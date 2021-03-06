/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
// vim:cindent:tabstop=2:expandtab:shiftwidth=2:
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   L. David Baron <dbaron@dbaron.org>
 *   Daniel Glazman <glazman@netscape.com>
 *   Ehsan Akhgari <ehsan.akhgari@gmail.com>
 *   Rob Arnold <robarnold@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

/*
 * style rule processor for CSS style sheets, responsible for selector
 * matching and cascading
 */

#include "nsCSSRuleProcessor.h"

#define PL_ARENA_CONST_ALIGN_MASK 7
#define NS_RULEHASH_ARENA_BLOCK_SIZE (256)
#include "plarena.h"

#include "nsCRT.h"
#include "nsIAtom.h"
#include "pldhash.h"
#include "nsHashtable.h"
#include "nsICSSPseudoComparator.h"
#include "nsCSSRuleProcessor.h"
#include "nsICSSStyleRule.h"
#include "nsICSSGroupRule.h"
#include "nsIDocument.h"
#include "nsPresContext.h"
#include "nsIEventStateManager.h"
#include "nsGkAtoms.h"
#include "nsString.h"
#include "nsUnicharUtils.h"
#include "nsVoidArray.h"
#include "nsDOMError.h"
#include "nsRuleWalker.h"
#include "nsCSSPseudoClasses.h"
#include "nsIContent.h"
#include "nsCOMPtr.h"
#include "nsHashKeys.h"
#include "nsStyleUtil.h"
#include "nsQuickSort.h"
#include "nsAttrValue.h"
#include "nsAttrName.h"
#include "nsILookAndFeel.h"
#include "nsWidgetsCID.h"
#include "nsServiceManagerUtils.h"
#include "nsTArray.h"
#include "nsContentUtils.h"
#include "nsIMediaList.h"
#include "nsCSSRules.h"
#include "nsIPrincipal.h"
#include "nsStyleSet.h"

#define VISITED_PSEUDO_PREF "layout.css.visited_links_enabled"

static PRBool gSupportVisitedPseudo = PR_TRUE;

static NS_DEFINE_CID(kLookAndFeelCID, NS_LOOKANDFEEL_CID);
static nsTArray< nsCOMPtr<nsIAtom> >* sSystemMetrics = 0;

struct RuleValue {
  /**
   * |RuleValue|s are constructed before they become part of the
   * |RuleHash|, to act as rule/selector pairs.  |Add| is called when
   * they are added to the |RuleHash|, and can be considered the second
   * half of the constructor.
   *
   * |RuleValue|s are added to the rule hash from highest weight/order
   * to lowest (since this is the fast way to build a singly linked
   * list), so the index used to remember the order is backwards.
   */
  RuleValue(nsICSSStyleRule* aRule, nsCSSSelector* aSelector)
    : mRule(aRule), mSelector(aSelector) {}

  RuleValue* Add(PRInt32 aBackwardIndex, RuleValue *aNext)
  {
    mBackwardIndex = aBackwardIndex;
    mNext = aNext;
    return this;
  }
    
  // CAUTION: ~RuleValue will never get called as RuleValues are arena
  // allocated and arena cleanup will take care of deleting memory.
  // Add code to RuleHash::~RuleHash to get it to call the destructor
  // if any more cleanup needs to happen.
  ~RuleValue()
  {
    // Rule values are arena allocated. No need for any deletion.
  }

  // Placement new to arena allocate the RuleValues
  void *operator new(size_t aSize, PLArenaPool &aArena) CPP_THROW_NEW {
    void *mem;
    PL_ARENA_ALLOCATE(mem, &aArena, aSize);
    return mem;
  }

  nsICSSStyleRule*  mRule;
  nsCSSSelector*    mSelector; // which of |mRule|'s selectors
  PRInt32           mBackwardIndex; // High index means low weight/order.
  RuleValue*        mNext;
};

// ------------------------------
// Rule hash table
//

// Uses any of the sets of ops below.
struct RuleHashTableEntry : public PLDHashEntryHdr {
  RuleValue *mRules; // linked list of |RuleValue|, null-terminated
};

static PLDHashNumber
RuleHash_CIHashKey(PLDHashTable *table, const void *key)
{
  nsIAtom *atom = const_cast<nsIAtom*>(static_cast<const nsIAtom*>(key));

  nsAutoString str;
  atom->ToString(str);
  ToUpperCase(str);
  return HashString(str);
}

typedef nsIAtom*
(* RuleHashGetKey) (PLDHashTable *table, const PLDHashEntryHdr *entry);

struct RuleHashTableOps {
  PLDHashTableOps ops;
  // Extra callback to avoid duplicating the matchEntry callback for
  // each table.  (There used to be a getKey callback in
  // PLDHashTableOps.)
  RuleHashGetKey getKey;
};

inline const RuleHashTableOps*
ToLocalOps(const PLDHashTableOps *aOps)
{
  return (const RuleHashTableOps*)
           (((const char*) aOps) - offsetof(RuleHashTableOps, ops));
}

static PRBool
RuleHash_CIMatchEntry(PLDHashTable *table, const PLDHashEntryHdr *hdr,
                      const void *key)
{
  nsIAtom *match_atom = const_cast<nsIAtom*>(static_cast<const nsIAtom*>
                                              (key));
  // Use our extra |getKey| callback to avoid code duplication.
  nsIAtom *entry_atom = ToLocalOps(table->ops)->getKey(table, hdr);

  // Check for case-sensitive match first.
  if (match_atom == entry_atom)
    return PR_TRUE;

  const char *match_str, *entry_str;
  match_atom->GetUTF8String(&match_str);
  entry_atom->GetUTF8String(&entry_str);

  return (nsCRT::strcasecmp(entry_str, match_str) == 0);
}

static PRBool
RuleHash_CSMatchEntry(PLDHashTable *table, const PLDHashEntryHdr *hdr,
                      const void *key)
{
  nsIAtom *match_atom = const_cast<nsIAtom*>(static_cast<const nsIAtom*>
                                              (key));
  // Use our extra |getKey| callback to avoid code duplication.
  nsIAtom *entry_atom = ToLocalOps(table->ops)->getKey(table, hdr);

  return match_atom == entry_atom;
}

static nsIAtom*
RuleHash_TagTable_GetKey(PLDHashTable *table, const PLDHashEntryHdr *hdr)
{
  const RuleHashTableEntry *entry =
    static_cast<const RuleHashTableEntry*>(hdr);
  return entry->mRules->mSelector->mTag;
}

static nsIAtom*
RuleHash_ClassTable_GetKey(PLDHashTable *table, const PLDHashEntryHdr *hdr)
{
  const RuleHashTableEntry *entry =
    static_cast<const RuleHashTableEntry*>(hdr);
  return entry->mRules->mSelector->mClassList->mAtom;
}

static nsIAtom*
RuleHash_IdTable_GetKey(PLDHashTable *table, const PLDHashEntryHdr *hdr)
{
  const RuleHashTableEntry *entry =
    static_cast<const RuleHashTableEntry*>(hdr);
  return entry->mRules->mSelector->mIDList->mAtom;
}

static PLDHashNumber
RuleHash_NameSpaceTable_HashKey(PLDHashTable *table, const void *key)
{
  return NS_PTR_TO_INT32(key);
}

static PRBool
RuleHash_NameSpaceTable_MatchEntry(PLDHashTable *table,
                                   const PLDHashEntryHdr *hdr,
                                   const void *key)
{
  const RuleHashTableEntry *entry =
    static_cast<const RuleHashTableEntry*>(hdr);

  return NS_PTR_TO_INT32(key) ==
         entry->mRules->mSelector->mNameSpace;
}

static const RuleHashTableOps RuleHash_TagTable_Ops = {
  {
  PL_DHashAllocTable,
  PL_DHashFreeTable,
  PL_DHashVoidPtrKeyStub,
  RuleHash_CSMatchEntry,
  PL_DHashMoveEntryStub,
  PL_DHashClearEntryStub,
  PL_DHashFinalizeStub,
  NULL
  },
  RuleHash_TagTable_GetKey
};

// Case-sensitive ops.
static const RuleHashTableOps RuleHash_ClassTable_CSOps = {
  {
  PL_DHashAllocTable,
  PL_DHashFreeTable,
  PL_DHashVoidPtrKeyStub,
  RuleHash_CSMatchEntry,
  PL_DHashMoveEntryStub,
  PL_DHashClearEntryStub,
  PL_DHashFinalizeStub,
  NULL
  },
  RuleHash_ClassTable_GetKey
};

// Case-insensitive ops.
static const RuleHashTableOps RuleHash_ClassTable_CIOps = {
  {
  PL_DHashAllocTable,
  PL_DHashFreeTable,
  RuleHash_CIHashKey,
  RuleHash_CIMatchEntry,
  PL_DHashMoveEntryStub,
  PL_DHashClearEntryStub,
  PL_DHashFinalizeStub,
  NULL
  },
  RuleHash_ClassTable_GetKey
};

// Case-sensitive ops.
static const RuleHashTableOps RuleHash_IdTable_CSOps = {
  {
  PL_DHashAllocTable,
  PL_DHashFreeTable,
  PL_DHashVoidPtrKeyStub,
  RuleHash_CSMatchEntry,
  PL_DHashMoveEntryStub,
  PL_DHashClearEntryStub,
  PL_DHashFinalizeStub,
  NULL
  },
  RuleHash_IdTable_GetKey
};

// Case-insensitive ops.
static const RuleHashTableOps RuleHash_IdTable_CIOps = {
  {
  PL_DHashAllocTable,
  PL_DHashFreeTable,
  RuleHash_CIHashKey,
  RuleHash_CIMatchEntry,
  PL_DHashMoveEntryStub,
  PL_DHashClearEntryStub,
  PL_DHashFinalizeStub,
  NULL
  },
  RuleHash_IdTable_GetKey
};

static const PLDHashTableOps RuleHash_NameSpaceTable_Ops = {
  PL_DHashAllocTable,
  PL_DHashFreeTable,
  RuleHash_NameSpaceTable_HashKey,
  RuleHash_NameSpaceTable_MatchEntry,
  PL_DHashMoveEntryStub,
  PL_DHashClearEntryStub,
  PL_DHashFinalizeStub,
  NULL,
};

#undef RULE_HASH_STATS
#undef PRINT_UNIVERSAL_RULES

#ifdef DEBUG_dbaron
#define RULE_HASH_STATS
#define PRINT_UNIVERSAL_RULES
#endif

#ifdef RULE_HASH_STATS
#define RULE_HASH_STAT_INCREMENT(var_) PR_BEGIN_MACRO ++(var_); PR_END_MACRO
#else
#define RULE_HASH_STAT_INCREMENT(var_) PR_BEGIN_MACRO PR_END_MACRO
#endif

// Enumerator callback function.
typedef void (*RuleEnumFunc)(nsICSSStyleRule* aRule,
                             nsCSSSelector* aSelector,
                             void *aData);

class RuleHash {
public:
  RuleHash(PRBool aQuirksMode);
  ~RuleHash();
  void PrependRule(RuleValue *aRuleInfo);
  void EnumerateAllRules(PRInt32 aNameSpace, nsIAtom* aTag, nsIAtom* aID,
                         const nsAttrValue* aClassList,
                         RuleEnumFunc aFunc, void* aData);
  void EnumerateTagRules(nsIAtom* aTag,
                         RuleEnumFunc aFunc, void* aData);
  PLArenaPool& Arena() { return mArena; }

protected:
  void PrependRuleToTable(PLDHashTable* aTable, const void* aKey,
                          RuleValue* aRuleInfo);
  void PrependUniversalRule(RuleValue* aRuleInfo);

  // All rule values in these hashtables are arena allocated
  PRInt32     mRuleCount;
  PLDHashTable mIdTable;
  PLDHashTable mClassTable;
  PLDHashTable mTagTable;
  PLDHashTable mNameSpaceTable;
  RuleValue *mUniversalRules;

  RuleValue** mEnumList;
  PRInt32     mEnumListSize;

  PLArenaPool mArena;

#ifdef RULE_HASH_STATS
  PRUint32    mUniversalSelectors;
  PRUint32    mNameSpaceSelectors;
  PRUint32    mTagSelectors;
  PRUint32    mClassSelectors;
  PRUint32    mIdSelectors;

  PRUint32    mElementsMatched;
  PRUint32    mPseudosMatched;

  PRUint32    mElementUniversalCalls;
  PRUint32    mElementNameSpaceCalls;
  PRUint32    mElementTagCalls;
  PRUint32    mElementClassCalls;
  PRUint32    mElementIdCalls;

  PRUint32    mPseudoTagCalls;
#endif // RULE_HASH_STATS
};

RuleHash::RuleHash(PRBool aQuirksMode)
  : mRuleCount(0),
    mUniversalRules(nsnull),
    mEnumList(nsnull), mEnumListSize(0)
#ifdef RULE_HASH_STATS
    ,
    mUniversalSelectors(0),
    mNameSpaceSelectors(0),
    mTagSelectors(0),
    mClassSelectors(0),
    mIdSelectors(0),
    mElementsMatched(0),
    mPseudosMatched(0),
    mElementUniversalCalls(0),
    mElementNameSpaceCalls(0),
    mElementTagCalls(0),
    mElementClassCalls(0),
    mElementIdCalls(0),
    mPseudoTagCalls(0)
#endif
{
  // Initialize our arena
  PL_INIT_ARENA_POOL(&mArena, "RuleHashArena", NS_RULEHASH_ARENA_BLOCK_SIZE);

  PL_DHashTableInit(&mTagTable, &RuleHash_TagTable_Ops.ops, nsnull,
                    sizeof(RuleHashTableEntry), 64);
  PL_DHashTableInit(&mIdTable,
                    aQuirksMode ? &RuleHash_IdTable_CIOps.ops
                                : &RuleHash_IdTable_CSOps.ops,
                    nsnull, sizeof(RuleHashTableEntry), 16);
  PL_DHashTableInit(&mClassTable,
                    aQuirksMode ? &RuleHash_ClassTable_CIOps.ops
                                : &RuleHash_ClassTable_CSOps.ops,
                    nsnull, sizeof(RuleHashTableEntry), 16);
  PL_DHashTableInit(&mNameSpaceTable, &RuleHash_NameSpaceTable_Ops, nsnull,
                    sizeof(RuleHashTableEntry), 16);
}

RuleHash::~RuleHash()
{
#ifdef RULE_HASH_STATS
  printf(
"RuleHash(%p):\n"
"  Selectors: Universal (%u) NameSpace(%u) Tag(%u) Class(%u) Id(%u)\n"
"  Content Nodes: Elements(%u) Pseudo-Elements(%u)\n"
"  Element Calls: Universal(%u) NameSpace(%u) Tag(%u) Class(%u) Id(%u)\n"
"  Pseudo-Element Calls: Tag(%u)\n",
         static_cast<void*>(this),
         mUniversalSelectors, mNameSpaceSelectors, mTagSelectors,
           mClassSelectors, mIdSelectors,
         mElementsMatched,
         mPseudosMatched,
         mElementUniversalCalls, mElementNameSpaceCalls, mElementTagCalls,
           mElementClassCalls, mElementIdCalls,
         mPseudoTagCalls);
#ifdef PRINT_UNIVERSAL_RULES
  {
    RuleValue* value = mUniversalRules;
    if (value) {
      printf("  Universal rules:\n");
      do {
        nsAutoString selectorText;
        PRUint32 lineNumber = value->mRule->GetLineNumber();
        nsCOMPtr<nsIStyleSheet> sheet;
        value->mRule->GetStyleSheet(*getter_AddRefs(sheet));
        nsCOMPtr<nsICSSStyleSheet> cssSheet = do_QueryInterface(sheet);
        value->mSelector->ToString(selectorText, cssSheet);

        printf("    line %d, %s\n",
               lineNumber, NS_ConvertUTF16toUTF8(selectorText).get());
        value = value->mNext;
      } while (value);
    }
  }
#endif // PRINT_UNIVERSAL_RULES
#endif // RULE_HASH_STATS
  // Rule Values are arena allocated no need to delete them. Their destructor
  // isn't doing any cleanup. So we dont even bother to enumerate through
  // the hash tables and call their destructors.
  if (nsnull != mEnumList) {
    delete [] mEnumList;
  }
  // delete arena for strings and small objects
  PL_DHashTableFinish(&mIdTable);
  PL_DHashTableFinish(&mClassTable);
  PL_DHashTableFinish(&mTagTable);
  PL_DHashTableFinish(&mNameSpaceTable);
  PL_FinishArenaPool(&mArena);
}

void RuleHash::PrependRuleToTable(PLDHashTable* aTable, const void* aKey,
                                  RuleValue* aRuleInfo)
{
  // Get a new or existing entry.
  RuleHashTableEntry *entry = static_cast<RuleHashTableEntry*>
                                         (PL_DHashTableOperate(aTable, aKey, PL_DHASH_ADD));
  if (!entry)
    return;
  entry->mRules = aRuleInfo->Add(mRuleCount++, entry->mRules);
}

void RuleHash::PrependUniversalRule(RuleValue *aRuleInfo)
{
  mUniversalRules = aRuleInfo->Add(mRuleCount++, mUniversalRules);
}

void RuleHash::PrependRule(RuleValue *aRuleInfo)
{
  nsCSSSelector *selector = aRuleInfo->mSelector;
  if (nsnull != selector->mIDList) {
    PrependRuleToTable(&mIdTable, selector->mIDList->mAtom, aRuleInfo);
    RULE_HASH_STAT_INCREMENT(mIdSelectors);
  }
  else if (nsnull != selector->mClassList) {
    PrependRuleToTable(&mClassTable, selector->mClassList->mAtom, aRuleInfo);
    RULE_HASH_STAT_INCREMENT(mClassSelectors);
  }
  else if (nsnull != selector->mTag) {
    PrependRuleToTable(&mTagTable, selector->mTag, aRuleInfo);
    RULE_HASH_STAT_INCREMENT(mTagSelectors);
  }
  else if (kNameSpaceID_Unknown != selector->mNameSpace) {
    PrependRuleToTable(&mNameSpaceTable,
                       NS_INT32_TO_PTR(selector->mNameSpace), aRuleInfo);
    RULE_HASH_STAT_INCREMENT(mNameSpaceSelectors);
  }
  else {  // universal tag selector
    PrependUniversalRule(aRuleInfo);
    RULE_HASH_STAT_INCREMENT(mUniversalSelectors);
  }
}

// this should cover practically all cases so we don't need to reallocate
#define MIN_ENUM_LIST_SIZE 8

#ifdef RULE_HASH_STATS
#define RULE_HASH_STAT_INCREMENT_LIST_COUNT(list_, var_) \
  do { ++(var_); (list_) = (list_)->mNext; } while (list_)
#else
#define RULE_HASH_STAT_INCREMENT_LIST_COUNT(list_, var_) \
  PR_BEGIN_MACRO PR_END_MACRO
#endif

void RuleHash::EnumerateAllRules(PRInt32 aNameSpace, nsIAtom* aTag,
                                 nsIAtom* aID, const nsAttrValue* aClassList,
                                 RuleEnumFunc aFunc, void* aData)
{
  PRInt32 classCount = aClassList ? aClassList->GetAtomCount() : 0;

  // assume 1 universal, tag, id, and namespace, rather than wasting
  // time counting
  PRInt32 testCount = classCount + 4;

  if (mEnumListSize < testCount) {
    delete [] mEnumList;
    mEnumListSize = PR_MAX(testCount, MIN_ENUM_LIST_SIZE);
    mEnumList = new RuleValue*[mEnumListSize];
  }

  PRInt32 valueCount = 0;
  RULE_HASH_STAT_INCREMENT(mElementsMatched);

  { // universal rules
    RuleValue* value = mUniversalRules;
    if (nsnull != value) {
      mEnumList[valueCount++] = value;
      RULE_HASH_STAT_INCREMENT_LIST_COUNT(value, mElementUniversalCalls);
    }
  }
  // universal rules within the namespace
  if (kNameSpaceID_Unknown != aNameSpace) {
    RuleHashTableEntry *entry = static_cast<RuleHashTableEntry*>
                                           (PL_DHashTableOperate(&mNameSpaceTable, NS_INT32_TO_PTR(aNameSpace),
                             PL_DHASH_LOOKUP));
    if (PL_DHASH_ENTRY_IS_BUSY(entry)) {
      RuleValue *value = entry->mRules;
      mEnumList[valueCount++] = value;
      RULE_HASH_STAT_INCREMENT_LIST_COUNT(value, mElementNameSpaceCalls);
    }
  }
  if (nsnull != aTag) {
    RuleHashTableEntry *entry = static_cast<RuleHashTableEntry*>
                                           (PL_DHashTableOperate(&mTagTable, aTag, PL_DHASH_LOOKUP));
    if (PL_DHASH_ENTRY_IS_BUSY(entry)) {
      RuleValue *value = entry->mRules;
      mEnumList[valueCount++] = value;
      RULE_HASH_STAT_INCREMENT_LIST_COUNT(value, mElementTagCalls);
    }
  }
  if (nsnull != aID) {
    RuleHashTableEntry *entry = static_cast<RuleHashTableEntry*>
                                           (PL_DHashTableOperate(&mIdTable, aID, PL_DHASH_LOOKUP));
    if (PL_DHASH_ENTRY_IS_BUSY(entry)) {
      RuleValue *value = entry->mRules;
      mEnumList[valueCount++] = value;
      RULE_HASH_STAT_INCREMENT_LIST_COUNT(value, mElementIdCalls);
    }
  }
  { // extra scope to work around compiler bugs with |for| scoping.
    for (PRInt32 index = 0; index < classCount; ++index) {
      RuleHashTableEntry *entry = static_cast<RuleHashTableEntry*>
                                             (PL_DHashTableOperate(&mClassTable, aClassList->AtomAt(index),
                             PL_DHASH_LOOKUP));
      if (PL_DHASH_ENTRY_IS_BUSY(entry)) {
        RuleValue *value = entry->mRules;
        mEnumList[valueCount++] = value;
        RULE_HASH_STAT_INCREMENT_LIST_COUNT(value, mElementClassCalls);
      }
    }
  }
  NS_ASSERTION(valueCount <= testCount, "values exceeded list size");

  if (valueCount > 0) {
    // Merge the lists while there are still multiple lists to merge.
    while (valueCount > 1) {
      PRInt32 valueIndex = 0;
      PRInt32 highestRuleIndex = mEnumList[valueIndex]->mBackwardIndex;
      for (PRInt32 index = 1; index < valueCount; ++index) {
        PRInt32 ruleIndex = mEnumList[index]->mBackwardIndex;
        if (ruleIndex > highestRuleIndex) {
          valueIndex = index;
          highestRuleIndex = ruleIndex;
        }
      }
      RuleValue *cur = mEnumList[valueIndex];
      (*aFunc)(cur->mRule, cur->mSelector, aData);
      RuleValue *next = cur->mNext;
      mEnumList[valueIndex] = next ? next : mEnumList[--valueCount];
    }

    // Fast loop over single value.
    RuleValue* value = mEnumList[0];
    do {
      (*aFunc)(value->mRule, value->mSelector, aData);
      value = value->mNext;
    } while (value);
  }
}

void RuleHash::EnumerateTagRules(nsIAtom* aTag, RuleEnumFunc aFunc, void* aData)
{
  RuleHashTableEntry *entry = static_cast<RuleHashTableEntry*>
                                         (PL_DHashTableOperate(&mTagTable, aTag, PL_DHASH_LOOKUP));

  RULE_HASH_STAT_INCREMENT(mPseudosMatched);
  if (PL_DHASH_ENTRY_IS_BUSY(entry)) {
    RuleValue *tagValue = entry->mRules;
    do {
      RULE_HASH_STAT_INCREMENT(mPseudoTagCalls);
      (*aFunc)(tagValue->mRule, tagValue->mSelector, aData);
      tagValue = tagValue->mNext;
    } while (tagValue);
  }
}

//--------------------------------

// Attribute selectors hash table.
struct AttributeSelectorEntry : public PLDHashEntryHdr {
  nsIAtom *mAttribute;
  nsVoidArray *mSelectors;
};

static void
AttributeSelectorClearEntry(PLDHashTable *table, PLDHashEntryHdr *hdr)
{
  AttributeSelectorEntry *entry = static_cast<AttributeSelectorEntry*>(hdr);
  delete entry->mSelectors;
  memset(entry, 0, table->entrySize);
}

static const PLDHashTableOps AttributeSelectorOps = {
  PL_DHashAllocTable,
  PL_DHashFreeTable,
  PL_DHashVoidPtrKeyStub,
  PL_DHashMatchEntryStub,
  PL_DHashMoveEntryStub,
  AttributeSelectorClearEntry,
  PL_DHashFinalizeStub,
  NULL
};


//--------------------------------

struct RuleCascadeData {
  RuleCascadeData(nsIAtom *aMedium, PRBool aQuirksMode)
    : mRuleHash(aQuirksMode),
      mStateSelectors(),
      mCacheKey(aMedium),
      mNext(nsnull)
  {
    PL_DHashTableInit(&mAttributeSelectors, &AttributeSelectorOps, nsnull,
                      sizeof(AttributeSelectorEntry), 16);
  }

  ~RuleCascadeData()
  {
    PL_DHashTableFinish(&mAttributeSelectors);
  }
  RuleHash          mRuleHash;
  nsVoidArray       mStateSelectors;
  nsVoidArray       mClassSelectors;
  nsVoidArray       mIDSelectors;
  PLDHashTable      mAttributeSelectors; // nsIAtom* -> nsVoidArray*

  nsTArray<nsFontFaceRuleContainer> mFontFaceRules;

  // Looks up or creates the appropriate list in |mAttributeSelectors|.
  // Returns null only on allocation failure.
  nsVoidArray* AttributeListFor(nsIAtom* aAttribute);

  nsMediaQueryResultCacheKey mCacheKey;
  RuleCascadeData*  mNext; // for a different medium
};

nsVoidArray*
RuleCascadeData::AttributeListFor(nsIAtom* aAttribute)
{
  AttributeSelectorEntry *entry = static_cast<AttributeSelectorEntry*>
                                             (PL_DHashTableOperate(&mAttributeSelectors, aAttribute, PL_DHASH_ADD));
  if (!entry)
    return nsnull;
  if (!entry->mSelectors) {
    if (!(entry->mSelectors = new nsVoidArray)) {
      PL_DHashTableRawRemove(&mAttributeSelectors, entry);
      return nsnull;
    }
    entry->mAttribute = aAttribute;
  }
  return entry->mSelectors;
}

// -------------------------------
// CSS Style rule processor implementation
//

nsCSSRuleProcessor::nsCSSRuleProcessor(const nsCOMArray<nsICSSStyleSheet>& aSheets,
                                       PRUint8 aSheetType)
  : mSheets(aSheets)
  , mRuleCascades(nsnull)
  , mLastPresContext(nsnull)
  , mSheetType(aSheetType)
{
  for (PRInt32 i = mSheets.Count() - 1; i >= 0; --i)
    mSheets[i]->AddRuleProcessor(this);
}

nsCSSRuleProcessor::~nsCSSRuleProcessor()
{
  for (PRInt32 i = mSheets.Count() - 1; i >= 0; --i)
    mSheets[i]->DropRuleProcessor(this);
  mSheets.Clear();
  ClearRuleCascades();
}

NS_IMPL_ISUPPORTS1(nsCSSRuleProcessor, nsIStyleRuleProcessor)

/* static */ void
nsCSSRuleProcessor::Startup()
{
  nsContentUtils::AddBoolPrefVarCache(VISITED_PSEUDO_PREF,
                                      &gSupportVisitedPseudo);
  // We want to default to true, not false as AddBoolPrefVarCache does.
  gSupportVisitedPseudo =
    nsContentUtils::GetBoolPref(VISITED_PSEUDO_PREF, PR_TRUE);
}

static PRBool
InitSystemMetrics()
{
  NS_ASSERTION(!sSystemMetrics, "already initialized");

  sSystemMetrics = new nsTArray< nsCOMPtr<nsIAtom> >;
  NS_ENSURE_TRUE(sSystemMetrics, PR_FALSE);

  nsresult rv;
  nsCOMPtr<nsILookAndFeel> lookAndFeel(do_GetService(kLookAndFeelCID, &rv));
  NS_ENSURE_SUCCESS(rv, PR_FALSE);

  PRInt32 metricResult;
  lookAndFeel->GetMetric(nsILookAndFeel::eMetric_ScrollArrowStyle, metricResult);
  if (metricResult & nsILookAndFeel::eMetric_ScrollArrowStartBackward) {
    sSystemMetrics->AppendElement(do_GetAtom("scrollbar-start-backward"));
  }
  if (metricResult & nsILookAndFeel::eMetric_ScrollArrowStartForward) {
    sSystemMetrics->AppendElement(do_GetAtom("scrollbar-start-forward"));
  }
  if (metricResult & nsILookAndFeel::eMetric_ScrollArrowEndBackward) {
    sSystemMetrics->AppendElement(do_GetAtom("scrollbar-end-backward"));
  }
  if (metricResult & nsILookAndFeel::eMetric_ScrollArrowEndForward) {
    sSystemMetrics->AppendElement(do_GetAtom("scrollbar-end-forward"));
  }

  lookAndFeel->GetMetric(nsILookAndFeel::eMetric_ScrollSliderStyle, metricResult);
  if (metricResult != nsILookAndFeel::eMetric_ScrollThumbStyleNormal) {
    sSystemMetrics->AppendElement(do_GetAtom("scrollbar-thumb-proportional"));
  }

  lookAndFeel->GetMetric(nsILookAndFeel::eMetric_ImagesInMenus, metricResult);
  if (metricResult) {
    sSystemMetrics->AppendElement(do_GetAtom("images-in-menus"));
  }

  rv = lookAndFeel->GetMetric(nsILookAndFeel::eMetric_WindowsDefaultTheme, metricResult);
  if (NS_SUCCEEDED(rv) && metricResult) {
    sSystemMetrics->AppendElement(do_GetAtom("windows-default-theme"));
  }

  rv = lookAndFeel->GetMetric(nsILookAndFeel::eMetric_MacGraphiteTheme, metricResult);
  if (NS_SUCCEEDED(rv) && metricResult) {
    sSystemMetrics->AppendElement(do_GetAtom("mac-graphite-theme"));
  }

  rv = lookAndFeel->GetMetric(nsILookAndFeel::eMetric_DWMCompositor, metricResult);
  if (NS_SUCCEEDED(rv) && metricResult) {
    sSystemMetrics->AppendElement(do_GetAtom("windows-compositor"));
  }

  rv = lookAndFeel->GetMetric(nsILookAndFeel::eMetric_WindowsClassic, metricResult);
  if (NS_SUCCEEDED(rv) && metricResult) {
    sSystemMetrics->AppendElement(do_GetAtom("windows-classic"));
  }
 
  return PR_TRUE;
}

/* static */ void
nsCSSRuleProcessor::FreeSystemMetrics()
{
  delete sSystemMetrics;
  sSystemMetrics = nsnull;
}

RuleProcessorData::RuleProcessorData(nsPresContext* aPresContext,
                                     nsIContent* aContent, 
                                     nsRuleWalker* aRuleWalker,
                                     nsCompatibility* aCompat /*= nsnull*/)
{
  MOZ_COUNT_CTOR(RuleProcessorData);

  NS_ASSERTION(!aContent || aContent->IsNodeOfType(nsINode::eELEMENT),
               "non-element leaked into SelectorMatches");

  mPresContext = aPresContext;
  mContent = aContent;
  mParentContent = nsnull;
  mRuleWalker = aRuleWalker;
  mScopedRoot = nsnull;

  mContentTag = nsnull;
  mContentID = nsnull;
  mHasAttributes = PR_FALSE;
  mIsHTMLContent = PR_FALSE;
  mIsLink = PR_FALSE;
  mLinkState = eLinkState_Unknown;
  mEventState = 0;
  mNameSpaceID = kNameSpaceID_Unknown;
  mPreviousSiblingData = nsnull;
  mParentData = nsnull;
  mLanguage = nsnull;
  mClasses = nsnull;
  mNthIndices[0][0] = -2;
  mNthIndices[0][1] = -2;
  mNthIndices[1][0] = -2;
  mNthIndices[1][1] = -2;

  // get the compat. mode (unless it is provided)
  // XXXbz is passing in the compat mode really that much of an optimization?
  if (aCompat) {
    mCompatMode = *aCompat;
  } else if (NS_LIKELY(mPresContext)) {
    mCompatMode = mPresContext->CompatibilityMode();
  } else {
    NS_ASSERTION(aContent, "Must have content");
    NS_ASSERTION(aContent->GetOwnerDoc(), "Must have document");
    mCompatMode = aContent->GetOwnerDoc()->GetCompatibilityMode();
  }

  if (aContent) {
    NS_ASSERTION(aContent->GetOwnerDoc(), "Document-less node here?");
    
    // get the tag and parent
    mContentTag = aContent->Tag();
    mParentContent = aContent->GetParent();

    // get the event state
    if (mPresContext) {
      mPresContext->EventStateManager()->GetContentState(aContent, mEventState);
    } else {
      mEventState = aContent->IntrinsicState();
    }

    // get the ID and classes for the content
    mContentID = aContent->GetID();
    mClasses = aContent->GetClasses();

    // see if there are attributes for the content
    mHasAttributes = aContent->GetAttrCount() > 0;

    // check for HTMLContent and Link status
    if (aContent->IsNodeOfType(nsINode::eHTML)) {
      mIsHTMLContent = PR_TRUE;
      // Note that we want to treat non-XML HTML content as XHTML for namespace
      // purposes, since html.css has that namespace declared.
      mNameSpaceID = kNameSpaceID_XHTML;
    } else {
      // get the namespace
      mNameSpaceID = aContent->GetNameSpaceID();
    }

    // if HTML content and it has some attributes, check for an HTML link
    // NOTE: optimization: cannot be a link if no attributes (since it needs an href)
    nsILinkHandler* linkHandler =
      mPresContext ? mPresContext->GetLinkHandler() : nsnull;
    if (mIsHTMLContent && mHasAttributes) {
      // check if it is an HTML Link
      if(nsStyleUtil::IsHTMLLink(aContent, mContentTag, linkHandler,
                                 &mLinkState)) {
        mIsLink = PR_TRUE;
      }
    } 

    // if not an HTML link, check for a simple xlink (cannot be both HTML link and xlink)
    // NOTE: optimization: cannot be an XLink if no attributes (since it needs an 
    if(!mIsLink &&
       mHasAttributes && 
       !(mIsHTMLContent || aContent->IsNodeOfType(nsINode::eXUL)) && 
       nsStyleUtil::IsLink(aContent, linkHandler, &mLinkState)) {
      mIsLink = PR_TRUE;
    } 
  }

  if (mLinkState == eLinkState_Visited && !gSupportVisitedPseudo) {
    mLinkState = eLinkState_Unvisited;
  }
}

RuleProcessorData::~RuleProcessorData()
{
  MOZ_COUNT_DTOR(RuleProcessorData);

  // Destroy potentially long chains of previous sibling and parent data
  // without more than one level of recursion.
  if (mPreviousSiblingData || mParentData) {
    nsAutoVoidArray destroyQueue;
    destroyQueue.AppendElement(this);

    do {
      RuleProcessorData *d = static_cast<RuleProcessorData*>
                                        (destroyQueue.FastElementAt(destroyQueue.Count() - 1));
      destroyQueue.RemoveElementAt(destroyQueue.Count() - 1);

      if (d->mPreviousSiblingData) {
        destroyQueue.AppendElement(d->mPreviousSiblingData);
        d->mPreviousSiblingData = nsnull;
      }
      if (d->mParentData) {
        destroyQueue.AppendElement(d->mParentData);
        d->mParentData = nsnull;
      }

      if (d != this)
        d->Destroy();
    } while (destroyQueue.Count());
  }

  delete mLanguage;
}

const nsString* RuleProcessorData::GetLang()
{
  if (!mLanguage) {
    mLanguage = new nsString();
    if (!mLanguage)
      return nsnull;
    for (nsIContent* content = mContent; content;
         content = content->GetParent()) {
      if (content->GetAttrCount() > 0) {
        // xml:lang has precedence over lang on HTML elements (see
        // XHTML1 section C.7).
        PRBool hasAttr = content->GetAttr(kNameSpaceID_XML, nsGkAtoms::lang,
                                          *mLanguage);
        if (!hasAttr && content->IsNodeOfType(nsINode::eHTML)) {
          hasAttr = content->GetAttr(kNameSpaceID_None, nsGkAtoms::lang,
                                     *mLanguage);
        }
        NS_ASSERTION(hasAttr || mLanguage->IsEmpty(),
                     "GetAttr that returns false should not make string non-empty");
        if (hasAttr) {
          break;
        }
      }
    }
  }
  return mLanguage;
}

static inline PRInt32
CSSNameSpaceID(nsIContent *aContent)
{
  return aContent->IsNodeOfType(nsINode::eHTML)
           ? kNameSpaceID_XHTML
           : aContent->GetNameSpaceID();
}

PRInt32
RuleProcessorData::GetNthIndex(PRBool aIsOfType, PRBool aIsFromEnd,
                               PRBool aCheckEdgeOnly)
{
  NS_ASSERTION(mParentContent, "caller should check mParentContent");
  NS_ASSERTION(!mPreviousSiblingData ||
               mPreviousSiblingData->mContent->IsNodeOfType(nsINode::eELEMENT),
               "Unexpected previous sibling data");

  PRInt32 &slot = mNthIndices[aIsOfType][aIsFromEnd];
  if (slot != -2 && (slot != -1 || aCheckEdgeOnly))
    return slot;

  if (mPreviousSiblingData &&
      (!aIsOfType ||
       (mPreviousSiblingData->mNameSpaceID == mNameSpaceID &&
        mPreviousSiblingData->mContentTag == mContentTag))) {
    slot = mPreviousSiblingData->mNthIndices[aIsOfType][aIsFromEnd];
    if (slot > 0) {
      slot += (aIsFromEnd ? -1 : 1);
      NS_ASSERTION(slot > 0, "How did that happen?");
      return slot;
    }
  }

  PRInt32 result = 1;
  nsIContent* parent = mParentContent;

  PRUint32 childCount = parent->GetChildCount();
  nsIContent * const * curChildPtr = parent->GetChildArray();

#ifdef DEBUG
  nsMutationGuard debugMutationGuard;
#endif  
  
  PRInt32 increment;
  nsIContent * const * stopPtr;
  if (aIsFromEnd) {
    stopPtr = curChildPtr - 1;
    curChildPtr = stopPtr + childCount;
    increment = -1;
  } else {
    increment = 1;
    stopPtr = curChildPtr + childCount;
  }

  for ( ; ; curChildPtr += increment) {
    if (curChildPtr == stopPtr) {
      // mContent is the root of an anonymous content subtree.
      result = 0; // special value to indicate that it is not at any index
      break;
    }
    nsIContent* child = *curChildPtr;
    if (child == mContent)
      break;
    if (child->IsNodeOfType(nsINode::eELEMENT) &&
        (!aIsOfType ||
         (child->Tag() == mContentTag &&
          CSSNameSpaceID(child) == mNameSpaceID))) {
      if (aCheckEdgeOnly) {
        // The caller only cares whether or not the result is 1, and we
        // now know it's not.
        result = -1;
        break;
      }
      ++result;
    }
  }

#ifdef DEBUG
  NS_ASSERTION(!debugMutationGuard.Mutated(0), "Unexpected mutations happened");
#endif  

  slot = result;
  return result;
}

static PRBool ValueIncludes(const nsSubstring& aValueList,
                            const nsSubstring& aValue,
                            const nsStringComparator& aComparator)
{
  const PRUnichar *p = aValueList.BeginReading(),
              *p_end = aValueList.EndReading();

  while (p < p_end) {
    // skip leading space
    while (p != p_end && nsContentUtils::IsHTMLWhitespace(*p))
      ++p;

    const PRUnichar *val_start = p;

    // look for space or end
    while (p != p_end && !nsContentUtils::IsHTMLWhitespace(*p))
      ++p;

    const PRUnichar *val_end = p;

    if (val_start < val_end &&
        aValue.Equals(Substring(val_start, val_end), aComparator))
      return PR_TRUE;

    ++p; // we know the next character is not whitespace
  }
  return PR_FALSE;
}

inline PRBool IsLinkPseudo(nsIAtom* aAtom)
{
  return PRBool ((nsCSSPseudoClasses::link == aAtom) || 
                 (nsCSSPseudoClasses::visited == aAtom) ||
                 (nsCSSPseudoClasses::mozAnyLink == aAtom));
}

// Return whether we should apply a "global" (i.e., universal-tag)
// selector for event states in quirks mode.  Note that
// |data.mIsLink| is checked separately by the caller, so we return
// false for |nsGkAtoms::a|, which here means a named anchor.
inline PRBool IsQuirkEventSensitive(nsIAtom *aContentTag)
{
  return PRBool ((nsGkAtoms::button == aContentTag) ||
                 (nsGkAtoms::img == aContentTag)    ||
                 (nsGkAtoms::input == aContentTag)  ||
                 (nsGkAtoms::label == aContentTag)  ||
                 (nsGkAtoms::select == aContentTag) ||
                 (nsGkAtoms::textarea == aContentTag));
}


static inline PRBool
IsSignificantChild(nsIContent* aChild, PRBool aTextIsSignificant,
                   PRBool aWhitespaceIsSignificant)
{
  return nsStyleUtil::IsSignificantChild(aChild, aTextIsSignificant,
                                         aWhitespaceIsSignificant);
}

// This function is to be called once we have fetched a value for an attribute
// whose namespace and name match those of aAttrSelector.  This function
// performs comparisons on the value only, based on aAttrSelector->mFunction.
static PRBool AttrMatchesValue(const nsAttrSelector* aAttrSelector,
                               const nsString& aValue)
{
  NS_PRECONDITION(aAttrSelector, "Must have an attribute selector");

  // http://lists.w3.org/Archives/Public/www-style/2008Apr/0038.html
  // *= (CONTAINSMATCH) ~= (INCLUDES) ^= (BEGINSMATCH) $= (ENDSMATCH)
  // all accept the empty string, but match nothing.
  if (aAttrSelector->mValue.IsEmpty() &&
      (aAttrSelector->mFunction == NS_ATTR_FUNC_INCLUDES ||
       aAttrSelector->mFunction == NS_ATTR_FUNC_ENDSMATCH ||
       aAttrSelector->mFunction == NS_ATTR_FUNC_BEGINSMATCH ||
       aAttrSelector->mFunction == NS_ATTR_FUNC_CONTAINSMATCH))
    return PR_FALSE;

  const nsDefaultStringComparator defaultComparator;
  const nsCaseInsensitiveStringComparator ciComparator;
  const nsStringComparator& comparator = aAttrSelector->mCaseSensitive
                ? static_cast<const nsStringComparator&>(defaultComparator)
                : static_cast<const nsStringComparator&>(ciComparator);
  switch (aAttrSelector->mFunction) {
    case NS_ATTR_FUNC_EQUALS: 
      return aValue.Equals(aAttrSelector->mValue, comparator);
    case NS_ATTR_FUNC_INCLUDES: 
      return ValueIncludes(aValue, aAttrSelector->mValue, comparator);
    case NS_ATTR_FUNC_DASHMATCH: 
      return nsStyleUtil::DashMatchCompare(aValue, aAttrSelector->mValue, comparator);
    case NS_ATTR_FUNC_ENDSMATCH:
      return StringEndsWith(aValue, aAttrSelector->mValue, comparator);
    case NS_ATTR_FUNC_BEGINSMATCH:
      return StringBeginsWith(aValue, aAttrSelector->mValue, comparator);
    case NS_ATTR_FUNC_CONTAINSMATCH:
      return FindInReadable(aAttrSelector->mValue, aValue, comparator);
    default:
      NS_NOTREACHED("Shouldn't be ending up here");
      return PR_FALSE;
  }
}

// NOTE: For |aStateMask| and |aAttribute| to work correctly, it's
// important that any change that changes multiple state bits and
// maybe an attribute include all those state bits and the attribute
// in the notification.  Otherwise, if multiple states change but we
// do separate notifications then we might determine the style is not
// state-dependent when it really is (e.g., determining that a
// :hover:active rule no longer matches when both states are unset).

// If |aForStyling| is false, we shouldn't mark slow-selector bits on nodes.

// |aDependence| has two functions:
//  * when non-null, it indicates that we're processing a negation,
//    which is done only when SelectorMatches calls itself recursively
//  * what it points to should be set to true whenever a test is skipped
//    because of aStateMask or aAttribute
static PRBool SelectorMatches(RuleProcessorData &data,
                              nsCSSSelector* aSelector,
                              PRInt32 aStateMask, // states NOT to test
                              nsIAtom* aAttribute, // attribute NOT to test
                              PRBool aForStyling,
                              PRBool* const aDependence = nsnull) 

{
  // namespace/tag match
  if ((kNameSpaceID_Unknown != aSelector->mNameSpace &&
       data.mNameSpaceID != aSelector->mNameSpace) ||
      (aSelector->mTag && aSelector->mTag != data.mContentTag)) {
    // optimization : bail out early if we can
    return PR_FALSE;
  }

  PRBool result = PR_TRUE;
  const PRBool isNegated = (aDependence != nsnull);
  // The selectors for which we set node bits are, unfortunately, early
  // in this function (because they're pseudo-classes, which are
  // generally quick to test, and thus earlier).  If they were later,
  // we'd probably avoid setting those bits in more cases where setting
  // them is unnecessary.
  const PRBool setNodeFlags = aForStyling && aStateMask == 0 && !aAttribute;

  // test for pseudo class match
  // first-child, root, lang, active, focus, hover, link, visited...
  // XXX disabled, enabled, selected, selection
  for (nsPseudoClassList* pseudoClass = aSelector->mPseudoClassList;
       pseudoClass && result; pseudoClass = pseudoClass->mNext) {
    PRInt32 stateToCheck = 0;
    if (nsCSSPseudoClasses::firstNode == pseudoClass->mAtom) {
      nsIContent *firstNode = nsnull;
      nsIContent *parent = data.mParentContent;
      if (parent) {
        if (setNodeFlags)
          parent->SetFlags(NODE_HAS_EDGE_CHILD_SELECTOR);

        PRInt32 index = -1;
        do {
          firstNode = parent->GetChildAt(++index);
          // stop at first non-comment and non-whitespace node
        } while (firstNode &&
                 !IsSignificantChild(firstNode, PR_TRUE, PR_FALSE));
      }
      result = (data.mContent == firstNode);
    }
    else if (nsCSSPseudoClasses::lastNode == pseudoClass->mAtom) {
      nsIContent *lastNode = nsnull;
      nsIContent *parent = data.mParentContent;
      if (parent) {
        if (setNodeFlags)
          parent->SetFlags(NODE_HAS_EDGE_CHILD_SELECTOR);

        PRUint32 index = parent->GetChildCount();
        do {
          lastNode = parent->GetChildAt(--index);
          // stop at first non-comment and non-whitespace node
        } while (lastNode &&
                 !IsSignificantChild(lastNode, PR_TRUE, PR_FALSE));
      }
      result = (data.mContent == lastNode);
    }
    else if (nsCSSPseudoClasses::firstChild == pseudoClass->mAtom ||
             nsCSSPseudoClasses::lastChild == pseudoClass->mAtom ||
             nsCSSPseudoClasses::onlyChild == pseudoClass->mAtom) {
      nsIContent *parent = data.mParentContent;
      if (parent) {
        const PRBool checkFirst =
          pseudoClass->mAtom != nsCSSPseudoClasses::lastChild;
        const PRBool checkLast =
          pseudoClass->mAtom != nsCSSPseudoClasses::firstChild;
        if (setNodeFlags)
          parent->SetFlags(NODE_HAS_EDGE_CHILD_SELECTOR);

        result = (!checkFirst ||
                  data.GetNthIndex(PR_FALSE, PR_FALSE, PR_TRUE) == 1) &&
                 (!checkLast ||
                  data.GetNthIndex(PR_FALSE, PR_TRUE, PR_TRUE) == 1);
      } else {
        result = PR_FALSE;
      }
    }
    else if (nsCSSPseudoClasses::nthChild == pseudoClass->mAtom ||
             nsCSSPseudoClasses::nthLastChild == pseudoClass->mAtom ||
             nsCSSPseudoClasses::nthOfType == pseudoClass->mAtom ||
             nsCSSPseudoClasses::nthLastOfType == pseudoClass->mAtom) {
      nsIContent *parent = data.mParentContent;
      if (parent) {
        PRBool isOfType =
          nsCSSPseudoClasses::nthOfType == pseudoClass->mAtom ||
          nsCSSPseudoClasses::nthLastOfType == pseudoClass->mAtom;
        PRBool isFromEnd =
          nsCSSPseudoClasses::nthLastChild == pseudoClass->mAtom ||
          nsCSSPseudoClasses::nthLastOfType == pseudoClass->mAtom;
        if (setNodeFlags) {
          if (isFromEnd)
            parent->SetFlags(NODE_HAS_SLOW_SELECTOR);
          else
            parent->SetFlags(NODE_HAS_SLOW_SELECTOR_NOAPPEND);
        }

        const PRInt32 index = data.GetNthIndex(isOfType, isFromEnd, PR_FALSE);
        if (index <= 0) {
          // Node is anonymous content (not really a child of its parent).
          result = PR_FALSE;
        } else {
          const PRInt32 a = pseudoClass->u.mNumbers[0];
          const PRInt32 b = pseudoClass->u.mNumbers[1];
          // result should be true if there exists n >= 0 such that
          // a * n + b == index.
          if (a == 0) {
            result = b == index;
          } else {
            // Integer division in C does truncation (towards 0).  So
            // check that the result is nonnegative, and that there was no
            // truncation.
            const PRInt32 n = (index - b) / a;
            result = n >= 0 && (a * n == index - b);
          }
        }
      } else {
        result = PR_FALSE;
      }
    }
    else if (nsCSSPseudoClasses::firstOfType == pseudoClass->mAtom ||
             nsCSSPseudoClasses::lastOfType == pseudoClass->mAtom ||
             nsCSSPseudoClasses::onlyOfType == pseudoClass->mAtom) {
      nsIContent *parent = data.mParentContent;
      if (parent) {
        const PRBool checkFirst =
          pseudoClass->mAtom != nsCSSPseudoClasses::lastOfType;
        const PRBool checkLast =
          pseudoClass->mAtom != nsCSSPseudoClasses::firstOfType;
        if (setNodeFlags) {
          if (checkLast)
            parent->SetFlags(NODE_HAS_SLOW_SELECTOR);
          else
            parent->SetFlags(NODE_HAS_SLOW_SELECTOR_NOAPPEND);
        }

        result = (!checkFirst ||
                  data.GetNthIndex(PR_TRUE, PR_FALSE, PR_TRUE) == 1) &&
                 (!checkLast ||
                  data.GetNthIndex(PR_TRUE, PR_TRUE, PR_TRUE) == 1);
      } else {
        result = PR_FALSE;
      }
    }
    else if (nsCSSPseudoClasses::empty == pseudoClass->mAtom ||
             nsCSSPseudoClasses::mozOnlyWhitespace == pseudoClass->mAtom) {
      nsIContent *child = nsnull;
      nsIContent *element = data.mContent;
      const PRBool isWhitespaceSignificant =
        nsCSSPseudoClasses::empty == pseudoClass->mAtom;
      PRInt32 index = -1;

      if (setNodeFlags)
        element->SetFlags(NODE_HAS_EMPTY_SELECTOR);

      do {
        child = element->GetChildAt(++index);
        // stop at first non-comment (and non-whitespace for
        // :-moz-only-whitespace) node        
      } while (child && !IsSignificantChild(child, PR_TRUE, isWhitespaceSignificant));
      result = (child == nsnull);
    }
    else if (nsCSSPseudoClasses::mozEmptyExceptChildrenWithLocalname == pseudoClass->mAtom) {
      NS_ASSERTION(pseudoClass->u.mString, "Must have string!");
      nsIContent *child = nsnull;
      nsIContent *element = data.mContent;
      PRInt32 index = -1;

      if (setNodeFlags)
        element->SetFlags(NODE_HAS_SLOW_SELECTOR);

      do {
        child = element->GetChildAt(++index);
      } while (child &&
               (!IsSignificantChild(child, PR_TRUE, PR_FALSE) ||
                (child->GetNameSpaceID() == element->GetNameSpaceID() &&
                 child->Tag()->Equals(nsDependentString(pseudoClass->u.mString)))));
      result = (child == nsnull);
    }
    else if (nsCSSPseudoClasses::mozSystemMetric == pseudoClass->mAtom) {
      if (!sSystemMetrics && !InitSystemMetrics()) {
        return PR_FALSE;
      }
      NS_ASSERTION(pseudoClass->u.mString, "Must have string!");
      nsCOMPtr<nsIAtom> metric = do_GetAtom(pseudoClass->u.mString);
      result = sSystemMetrics->IndexOf(metric) !=
               sSystemMetrics->NoIndex;
    }
    else if (nsCSSPseudoClasses::mozHasHandlerRef == pseudoClass->mAtom) {
      nsIContent *child = nsnull;
      nsIContent *element = data.mContent;
      PRInt32 index = -1;

      result = PR_FALSE;
      if (element) {
        do {
          child = element->GetChildAt(++index);
          if (child && child->IsNodeOfType(nsINode::eHTML) &&
              child->Tag() == nsGkAtoms::param &&
              child->AttrValueIs(kNameSpaceID_None, nsGkAtoms::name,
                                 NS_LITERAL_STRING("pluginurl"), eIgnoreCase)) {
            result = PR_TRUE;
            break;
          }
        } while (child);
      }
    }
    else if (nsCSSPseudoClasses::root == pseudoClass->mAtom) {
      result = (data.mParentContent == nsnull &&
                data.mContent &&
                data.mContent ==
                  data.mContent->GetOwnerDoc()->GetRootContent());
    }
    else if (nsCSSPseudoClasses::mozBoundElement == pseudoClass->mAtom) {
      // XXXldb How do we know where the selector came from?  And what
      // if there are multiple bindings, and we should be matching the
      // outer one?
      result = (data.mScopedRoot && data.mScopedRoot == data.mContent);
    }
    else if (nsCSSPseudoClasses::lang == pseudoClass->mAtom) {
      NS_ASSERTION(nsnull != pseudoClass->u.mString, "null lang parameter");
      result = PR_FALSE;
      if (pseudoClass->u.mString && *pseudoClass->u.mString) {
        // We have to determine the language of the current element.  Since
        // this is currently no property and since the language is inherited
        // from the parent we have to be prepared to look at all parent
        // nodes.  The language itself is encoded in the LANG attribute.
        const nsString* lang = data.GetLang();
        if (lang && !lang->IsEmpty()) { // null check for out-of-memory
          result = nsStyleUtil::DashMatchCompare(*lang,
                                    nsDependentString(pseudoClass->u.mString), 
                                    nsCaseInsensitiveStringComparator());
        }
        else if (data.mContent) {
          nsIDocument* doc = data.mContent->GetDocument();
          if (doc) {
            // Try to get the language from the HTTP header or if this
            // is missing as well from the preferences.
            // The content language can be a comma-separated list of
            // language codes.
            nsAutoString language;
            doc->GetContentLanguage(language);

            nsDependentString langString(pseudoClass->u.mString);
            language.StripWhitespace();
            PRInt32 begin = 0;
            PRInt32 len = language.Length();
            while (begin < len) {
              PRInt32 end = language.FindChar(PRUnichar(','), begin);
              if (end == kNotFound) {
                end = len;
              }
              if (nsStyleUtil::DashMatchCompare(Substring(language, begin, end-begin),
                                   langString,
                                   nsCaseInsensitiveStringComparator())) {
                result = PR_TRUE;
                break;
              }
              begin = end + 1;
            }
          }
        }
      }
    } else if (nsCSSPseudoClasses::active == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_ACTIVE;
    }
    else if (nsCSSPseudoClasses::focus == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_FOCUS;
    }
    else if (nsCSSPseudoClasses::hover == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_HOVER;
    }
    else if (nsCSSPseudoClasses::mozDragOver == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_DRAGOVER;
    }
    else if (nsCSSPseudoClasses::target == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_URLTARGET;
    }
    else if (IsLinkPseudo(pseudoClass->mAtom)) {
      if (data.mIsLink) {
        if (nsCSSPseudoClasses::mozAnyLink == pseudoClass->mAtom) {
          result = PR_TRUE;
        }
        else {
          NS_ASSERTION(nsCSSPseudoClasses::link == pseudoClass->mAtom ||
                       nsCSSPseudoClasses::visited == pseudoClass->mAtom,
                       "somebody changed IsLinkPseudo");
          NS_ASSERTION(data.mLinkState == eLinkState_Unvisited ||
                       data.mLinkState == eLinkState_Visited,
                       "unexpected link state for mIsLink");
          if (aStateMask & NS_EVENT_STATE_VISITED) {
            result = PR_TRUE;
            if (aDependence)
              *aDependence = PR_TRUE;
          } else {
            result = ((eLinkState_Unvisited == data.mLinkState) ==
                      (nsCSSPseudoClasses::link == pseudoClass->mAtom));
          }
        }
      }
      else {
        result = PR_FALSE;  // not a link
      }
    }
    else if (nsCSSPseudoClasses::checked == pseudoClass->mAtom) {
      // This pseudoclass matches the selected state on the following elements:
      //  <option>
      //  <input type=checkbox>
      //  <input type=radio>
      stateToCheck = NS_EVENT_STATE_CHECKED;
    }
    else if (nsCSSPseudoClasses::enabled == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_ENABLED;
    }
    else if (nsCSSPseudoClasses::disabled == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_DISABLED;
    }    
    else if (nsCSSPseudoClasses::mozBroken == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_BROKEN;
    }
    else if (nsCSSPseudoClasses::mozUserDisabled == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_USERDISABLED;
    }
    else if (nsCSSPseudoClasses::mozSuppressed == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_SUPPRESSED;
    }
    else if (nsCSSPseudoClasses::mozLoading == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_LOADING;
    }
    else if (nsCSSPseudoClasses::mozTypeUnsupported == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_TYPE_UNSUPPORTED;
    }
    else if (nsCSSPseudoClasses::mozHandlerDisabled == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_HANDLER_DISABLED;
    }
    else if (nsCSSPseudoClasses::mozHandlerBlocked == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_HANDLER_BLOCKED;
    }
    else if (nsCSSPseudoClasses::defaultPseudo == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_DEFAULT;
    }
    else if (nsCSSPseudoClasses::required == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_REQUIRED;
    }
    else if (nsCSSPseudoClasses::optional == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_OPTIONAL;
    }
    else if (nsCSSPseudoClasses::valid == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_VALID;
    }
    else if (nsCSSPseudoClasses::invalid == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_INVALID;
    }
    else if (nsCSSPseudoClasses::inRange == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_INRANGE;
    }
    else if (nsCSSPseudoClasses::outOfRange == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_OUTOFRANGE;
    }
    else if (nsCSSPseudoClasses::mozReadOnly == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_MOZ_READONLY;
    }
    else if (nsCSSPseudoClasses::mozReadWrite == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_MOZ_READWRITE;
    }
    else if (nsCSSPseudoClasses::mozIsHTML == pseudoClass->mAtom) {
      result = data.mIsHTMLContent &&
        data.mContent->GetNameSpaceID() == kNameSpaceID_None;
    }
#ifdef MOZ_MATHML
    else if (nsCSSPseudoClasses::mozMathIncrementScriptLevel == pseudoClass->mAtom) {
      stateToCheck = NS_EVENT_STATE_INCREMENT_SCRIPT_LEVEL;
    }
#endif
    else {
      NS_ERROR("CSS parser parsed a pseudo-class that we do not handle");
      result = PR_FALSE;  // unknown pseudo class
    }
    if (stateToCheck) {
      // check if the element is event-sensitive for :hover and :active
      if ((stateToCheck & (NS_EVENT_STATE_HOVER | NS_EVENT_STATE_ACTIVE)) &&
          data.mCompatMode == eCompatibility_NavQuirks &&
          // global selector (but don't check .class):
          !aSelector->mTag && !aSelector->mIDList && !aSelector->mAttrList &&
          // This (or the other way around) both make :not() asymmetric
          // in quirks mode (and it's hard to work around since we're
          // testing the current mNegations, not the first
          // (unnegated)). This at least makes it closer to the spec.
          !isNegated &&
          // important for |IsQuirkEventSensitive|:
          data.mIsHTMLContent && !data.mIsLink &&
          !IsQuirkEventSensitive(data.mContentTag)) {
        // In quirks mode, only make certain elements sensitive to
        // selectors ":hover" and ":active".
        result = PR_FALSE;
      } else {
        if (aStateMask & stateToCheck) {
          result = PR_TRUE;
          if (aDependence)
            *aDependence = PR_TRUE;
        } else {
          result = (0 != (data.mEventState & stateToCheck));
        }
      }
    }
  }

  if (result && aSelector->mAttrList) {
    // test for attribute match
    if (!data.mHasAttributes && !aAttribute) {
      // if no attributes on the content, no match
      result = PR_FALSE;
    } else {
      NS_ASSERTION(data.mContent,
                   "Must have content if either data.mHasAttributes or "
                   "aAttribute is set!");
      result = PR_TRUE;
      nsAttrSelector* attr = aSelector->mAttrList;
      do {
        if (attr->mAttr == aAttribute) {
          // XXX we should really have a namespace, not just an attr
          // name, in HasAttributeDependentStyle!
          result = PR_TRUE;
          if (aDependence)
            *aDependence = PR_TRUE;
        }
        else if (attr->mNameSpace == kNameSpaceID_Unknown) {
          // Attr selector with a wildcard namespace.  We have to examine all
          // the attributes on our content node....  This sort of selector is
          // essentially a boolean OR, over all namespaces, of equivalent attr
          // selectors with those namespaces.  So to evaluate whether it
          // matches, evaluate for each namespace (the only namespaces that
          // have a chance at matching, of course, are ones that the element
          // actually has attributes in), short-circuiting if we ever match.
          PRUint32 attrCount = data.mContent->GetAttrCount();
          result = PR_FALSE;
          for (PRUint32 i = 0; i < attrCount; ++i) {
            const nsAttrName* attrName =
              data.mContent->GetAttrNameAt(i);
            NS_ASSERTION(attrName, "GetAttrCount lied or GetAttrNameAt failed");
            if (attrName->LocalName() != attr->mAttr) {
              continue;
            }
            if (attr->mFunction == NS_ATTR_FUNC_SET) {
              result = PR_TRUE;
            } else {
              nsAutoString value;
#ifdef DEBUG
              PRBool hasAttr =
#endif
                data.mContent->GetAttr(attrName->NamespaceID(),
                                       attrName->LocalName(), value);
              NS_ASSERTION(hasAttr, "GetAttrNameAt lied");
              result = AttrMatchesValue(attr, value);
            }

            // At this point |result| has been set by us
            // explicitly in this loop.  If it's PR_FALSE, we may still match
            // -- the content may have another attribute with the same name but
            // in a different namespace.  But if it's PR_TRUE, we are done (we
            // can short-circuit the boolean OR described above).
            if (result) {
              break;
            }
          }
        }
        else if (attr->mFunction == NS_ATTR_FUNC_EQUALS) {
          result =
            data.mContent->
              AttrValueIs(attr->mNameSpace, attr->mAttr, attr->mValue,
                          attr->mCaseSensitive ? eCaseMatters : eIgnoreCase);
        }
        else if (!data.mContent->HasAttr(attr->mNameSpace, attr->mAttr)) {
          result = PR_FALSE;
        }
        else if (attr->mFunction != NS_ATTR_FUNC_SET) {
          nsAutoString value;
#ifdef DEBUG
          PRBool hasAttr =
#endif
              data.mContent->GetAttr(attr->mNameSpace, attr->mAttr, value);
          NS_ASSERTION(hasAttr, "HasAttr lied");
          result = AttrMatchesValue(attr, value);
        }
        
        attr = attr->mNext;
      } while (attr && result);
    }
  }
  nsAtomList* IDList = aSelector->mIDList;
  if (result && IDList) {
    // test for ID match
    result = PR_FALSE;

    if (aAttribute && aAttribute == data.mContent->GetIDAttributeName()) {
      result = PR_TRUE;
      if (aDependence)
        *aDependence = PR_TRUE;
    }
    else if (nsnull != data.mContentID) {
      // case sensitivity: bug 93371
      const PRBool isCaseSensitive =
        data.mCompatMode != eCompatibility_NavQuirks;

      result = PR_TRUE;
      if (isCaseSensitive) {
        do {
          if (IDList->mAtom != data.mContentID) {
            result = PR_FALSE;
            break;
          }
          IDList = IDList->mNext;
        } while (IDList);
      } else {
        const char* id1Str;
        data.mContentID->GetUTF8String(&id1Str);
        nsDependentCString id1(id1Str);
        do {
          const char* id2Str;
          IDList->mAtom->GetUTF8String(&id2Str);
          if (!id1.Equals(id2Str, nsCaseInsensitiveCStringComparator())) {
            result = PR_FALSE;
            break;
          }
          IDList = IDList->mNext;
        } while (IDList);
      }
    }
  }
    
  if (result && aSelector->mClassList) {
    // test for class match
    if (aAttribute && aAttribute == data.mContent->GetClassAttributeName()) {
      result = PR_TRUE;
      if (aDependence)
        *aDependence = PR_TRUE;
    }
    else {
      // case sensitivity: bug 93371
      const PRBool isCaseSensitive =
        data.mCompatMode != eCompatibility_NavQuirks;

      nsAtomList* classList = aSelector->mClassList;
      const nsAttrValue *elementClasses = data.mClasses;
      while (nsnull != classList) {
        if (!(elementClasses && elementClasses->Contains(classList->mAtom, isCaseSensitive ? eCaseMatters : eIgnoreCase))) {
          result = PR_FALSE;
          break;
        }
        classList = classList->mNext;
      }
    }
  }
  
  // apply SelectorMatches to the negated selectors in the chain
  if (!isNegated) {
    for (nsCSSSelector *negation = aSelector->mNegations;
         result && negation; negation = negation->mNegations) {
      PRBool dependence = PR_FALSE;
      result = !SelectorMatches(data, negation, aStateMask,
                                aAttribute, aForStyling, &dependence);
      // If the selector does match due to the dependence on aStateMask
      // or aAttribute, then we want to keep result true so that the
      // final result of SelectorMatches is true.  Doing so tells
      // StateEnumFunc or AttributeEnumFunc that there is a dependence
      // on the state or attribute.
      result = result || dependence;
    }
  }
  return result;
}

#undef STATE_CHECK

// Right now, there are four operators:
//   PRUnichar(0), the descendant combinator, is greedy
//   '~', the indirect adjacent sibling combinator, is greedy
//   '+' and '>', the direct adjacent sibling and child combinators, are not
#define NS_IS_GREEDY_OPERATOR(ch) (ch == PRUnichar(0) || ch == PRUnichar('~'))

static PRBool SelectorMatchesTree(RuleProcessorData& aPrevData,
                                  nsCSSSelector* aSelector,
                                  PRBool aForStyling) 
{
  nsCSSSelector* selector = aSelector;
  RuleProcessorData* prevdata = &aPrevData;
  while (selector) { // check compound selectors
    // If we don't already have a RuleProcessorData for the next
    // appropriate content (whether parent or previous sibling), create
    // one.

    // for adjacent sibling combinators, the content to test against the
    // selector is the previous sibling *element*
    RuleProcessorData* data;
    if (PRUnichar('+') == selector->mOperator ||
        PRUnichar('~') == selector->mOperator) {
      data = prevdata->mPreviousSiblingData;
      if (!data) {
        nsIContent* content = prevdata->mContent;
        nsIContent* parent = content->GetParent();
        if (parent) {
          parent->SetFlags(NODE_HAS_SLOW_SELECTOR_NOAPPEND);

          PRInt32 index = parent->IndexOf(content);
          while (0 <= --index) {
            content = parent->GetChildAt(index);
            if (content->IsNodeOfType(nsINode::eELEMENT)) {
              data = RuleProcessorData::Create(prevdata->mPresContext, content,
                                               prevdata->mRuleWalker,
                                               prevdata->mCompatMode);
              prevdata->mPreviousSiblingData = data;    
              break;
            }
          }
        }
      }
    }
    // for descendant combinators and child combinators, the content
    // to test against is the parent
    else {
      data = prevdata->mParentData;
      if (!data) {
        nsIContent *content = prevdata->mContent->GetParent();
        // GetParent could return a document fragment; we only want
        // element parents.
        if (content && content->IsNodeOfType(nsINode::eELEMENT)) {
          data = RuleProcessorData::Create(prevdata->mPresContext, content,
                                           prevdata->mRuleWalker,
                                           prevdata->mCompatMode);
          prevdata->mParentData = data;    
        }
      }
    }
    if (! data) {
      return PR_FALSE;
    }
    if (SelectorMatches(*data, selector, 0, nsnull, aForStyling)) {
      // to avoid greedy matching, we need to recur if this is a
      // descendant or general sibling combinator and the next
      // combinator is different, but we can make an exception for
      // sibling, then parent, since a sibling's parent is always the
      // same.
      if ((NS_IS_GREEDY_OPERATOR(selector->mOperator)) &&
          (selector->mNext) &&
          (selector->mNext->mOperator != selector->mOperator) &&
          !(selector->mOperator == '~' &&
            selector->mNext->mOperator == PRUnichar(0))) {

        // pretend the selector didn't match, and step through content
        // while testing the same selector

        // This approach is slightly strange in that when it recurs
        // it tests from the top of the content tree, down.  This
        // doesn't matter much for performance since most selectors
        // don't match.  (If most did, it might be faster...)
        if (SelectorMatchesTree(*data, selector, aForStyling)) {
          return PR_TRUE;
        }
      }
      selector = selector->mNext;
    }
    else {
      // for adjacent sibling and child combinators, if we didn't find
      // a match, we're done
      if (!NS_IS_GREEDY_OPERATOR(selector->mOperator)) {
        return PR_FALSE;  // parent was required to match
      }
    }
    prevdata = data;
  }
  return PR_TRUE; // all the selectors matched.
}

static void ContentEnumFunc(nsICSSStyleRule* aRule, nsCSSSelector* aSelector,
                            void* aData)
{
  ElementRuleProcessorData* data = (ElementRuleProcessorData*)aData;

  if (SelectorMatches(*data, aSelector, 0, nsnull, PR_TRUE)) {
    nsCSSSelector *next = aSelector->mNext;
    if (!next || SelectorMatchesTree(*data, next, PR_TRUE)) {
      // for performance, require that every implementation of
      // nsICSSStyleRule return the same pointer for nsIStyleRule (why
      // would anything multiply inherit nsIStyleRule anyway?)
#ifdef DEBUG
      nsCOMPtr<nsIStyleRule> iRule = do_QueryInterface(aRule);
      NS_ASSERTION(static_cast<nsIStyleRule*>(aRule) == iRule.get(),
                   "Please fix QI so this performance optimization is valid");
#endif
      data->mRuleWalker->Forward(static_cast<nsIStyleRule*>(aRule));
      // nsStyleSet will deal with the !important rule
    }
  }
}

NS_IMETHODIMP
nsCSSRuleProcessor::RulesMatching(ElementRuleProcessorData *aData)
{
  NS_PRECONDITION(aData->mContent->IsNodeOfType(nsINode::eELEMENT),
                  "content must be element");

  RuleCascadeData* cascade = GetRuleCascade(aData->mPresContext);

  if (cascade) {
    cascade->mRuleHash.EnumerateAllRules(aData->mNameSpaceID,
                                         aData->mContentTag,
                                         aData->mContentID,
                                         aData->mClasses,
                                         ContentEnumFunc,
                                         aData);
  }
  return NS_OK;
}

static void PseudoEnumFunc(nsICSSStyleRule* aRule, nsCSSSelector* aSelector,
                           void* aData)
{
  PseudoRuleProcessorData* data = (PseudoRuleProcessorData*)aData;

  NS_ASSERTION(aSelector->mTag == data->mPseudoTag, "RuleHash failure");
  PRBool matches = PR_TRUE;
  if (data->mComparator)
    data->mComparator->PseudoMatches(data->mPseudoTag, aSelector, &matches);

  if (matches) {
    nsCSSSelector *selector = aSelector->mNext;

    if (selector) { // test next selector specially
      if (PRUnichar('+') == selector->mOperator) {
        return; // not valid here, can't match
      }
      if (SelectorMatches(*data, selector, 0, nsnull, PR_TRUE)) {
        selector = selector->mNext;
      }
      else {
        if (PRUnichar('>') == selector->mOperator) {
          return; // immediate parent didn't match
        }
      }
    }

    if (selector && 
        (! SelectorMatchesTree(*data, selector, PR_TRUE))) {
      return; // remaining selectors didn't match
    }

    // for performance, require that every implementation of
    // nsICSSStyleRule return the same pointer for nsIStyleRule (why
    // would anything multiply inherit nsIStyleRule anyway?)
#ifdef DEBUG
    nsCOMPtr<nsIStyleRule> iRule = do_QueryInterface(aRule);
    NS_ASSERTION(static_cast<nsIStyleRule*>(aRule) == iRule.get(),
                 "Please fix QI so this performance optimization is valid");
#endif
    data->mRuleWalker->Forward(static_cast<nsIStyleRule*>(aRule));
    // nsStyleSet will deal with the !important rule
  }
}

NS_IMETHODIMP
nsCSSRuleProcessor::RulesMatching(PseudoRuleProcessorData* aData)
{
  NS_PRECONDITION(!aData->mContent ||
                  aData->mContent->IsNodeOfType(nsINode::eELEMENT),
                  "content (if present) must be element");

  RuleCascadeData* cascade = GetRuleCascade(aData->mPresContext);

  if (cascade) {
    cascade->mRuleHash.EnumerateTagRules(aData->mPseudoTag,
                                         PseudoEnumFunc, aData);
  }
  return NS_OK;
}

inline PRBool
IsSiblingOperator(PRUnichar oper)
{
  return oper == PRUnichar('+') || oper == PRUnichar('~');
}

struct StateEnumData {
  StateEnumData(StateRuleProcessorData *aData)
    : data(aData), change(nsReStyleHint(0)) {}

  StateRuleProcessorData *data;
  nsReStyleHint change;
};

static PRBool StateEnumFunc(void* aSelector, void* aData)
{
  StateEnumData *enumData = static_cast<StateEnumData*>(aData);
  StateRuleProcessorData *data = enumData->data;
  nsCSSSelector* selector = static_cast<nsCSSSelector*>(aSelector);

  nsReStyleHint possibleChange = IsSiblingOperator(selector->mOperator) ?
    eReStyle_LaterSiblings : eReStyle_Self;

  // If enumData->change already includes all the bits of possibleChange, don't
  // bother calling SelectorMatches, since even if it returns false
  // enumData->change won't change.
  if ((possibleChange & ~(enumData->change)) &&
      SelectorMatches(*data, selector, data->mStateMask, nsnull, PR_TRUE) &&
      SelectorMatchesTree(*data, selector->mNext, PR_TRUE)) {
    enumData->change = nsReStyleHint(enumData->change | possibleChange);
  }

  return PR_TRUE;
}

NS_IMETHODIMP
nsCSSRuleProcessor::HasStateDependentStyle(StateRuleProcessorData* aData,
                                           nsReStyleHint* aResult)
{
  NS_PRECONDITION(aData->mContent->IsNodeOfType(nsINode::eELEMENT),
                  "content must be element");

  RuleCascadeData* cascade = GetRuleCascade(aData->mPresContext);

  // Look up the content node in the state rule list, which points to
  // any (CSS2 definition) simple selector (whether or not it is the
  // subject) that has a state pseudo-class on it.  This means that this
  // code will be matching selectors that aren't real selectors in any
  // stylesheet (e.g., if there is a selector "body > p:hover > a", then
  // "body > p:hover" will be in |cascade->mStateSelectors|).  Note that
  // |IsStateSelector| below determines which selectors are in
  // |cascade->mStateSelectors|.
  StateEnumData data(aData);
  if (cascade)
    cascade->mStateSelectors.EnumerateForwards(StateEnumFunc, &data);
  *aResult = data.change;
  return NS_OK;
}

struct AttributeEnumData {
  AttributeEnumData(AttributeRuleProcessorData *aData)
    : data(aData), change(nsReStyleHint(0)) {}

  AttributeRuleProcessorData *data;
  nsReStyleHint change;
};


static PRBool AttributeEnumFunc(void* aSelector, void* aData)
{
  AttributeEnumData *enumData = static_cast<AttributeEnumData*>(aData);
  AttributeRuleProcessorData *data = enumData->data;
  nsCSSSelector* selector = static_cast<nsCSSSelector*>(aSelector);

  nsReStyleHint possibleChange = IsSiblingOperator(selector->mOperator) ?
    eReStyle_LaterSiblings : eReStyle_Self;

  // If enumData->change already includes all the bits of possibleChange, don't
  // bother calling SelectorMatches, since even if it returns false
  // enumData->change won't change.
  if ((possibleChange & ~(enumData->change)) &&
      SelectorMatches(*data, selector, data->mStateMask, data->mAttribute,
                      PR_TRUE) &&
      SelectorMatchesTree(*data, selector->mNext, PR_TRUE)) {
    enumData->change = nsReStyleHint(enumData->change | possibleChange);
  }

  return PR_TRUE;
}

NS_IMETHODIMP
nsCSSRuleProcessor::HasAttributeDependentStyle(AttributeRuleProcessorData* aData,
                                               nsReStyleHint* aResult)
{
  NS_PRECONDITION(aData->mContent->IsNodeOfType(nsINode::eELEMENT),
                  "content must be element");

  AttributeEnumData data(aData);

  // Since we always have :-moz-any-link (and almost always have :link
  // and :visited rules from prefs), rather than hacking AddRule below
  // to add |href| to the hash, we'll just handle it here.
  if (aData->mAttribute == nsGkAtoms::href &&
      aData->mIsHTMLContent &&
      (aData->mContentTag == nsGkAtoms::a ||
       aData->mContentTag == nsGkAtoms::area ||
       aData->mContentTag == nsGkAtoms::link)) {
    data.change = nsReStyleHint(data.change | eReStyle_Self);
  }
  // XXX What about XLinks?
#ifdef MOZ_SVG
  // XXX should really check the attribute namespace is XLink
  if (aData->mAttribute == nsGkAtoms::href &&
      aData->mNameSpaceID == kNameSpaceID_SVG &&
      aData->mContentTag == nsGkAtoms::a) {
    data.change = nsReStyleHint(data.change | eReStyle_Self);
  }
#endif
  // XXXbz now that :link and :visited are also states, do we need a
  // similar optimization in HasStateDependentStyle?

  RuleCascadeData* cascade = GetRuleCascade(aData->mPresContext);

  // We do the same thing for attributes that we do for state selectors
  // (see HasStateDependentStyle), except that instead of one big list
  // we have a hashtable with a per-attribute list.

  if (cascade) {
    if (aData->mAttribute == aData->mContent->GetIDAttributeName()) {
      cascade->mIDSelectors.EnumerateForwards(AttributeEnumFunc, &data);
    }
    
    if (aData->mAttribute == aData->mContent->GetClassAttributeName()) {
      cascade->mClassSelectors.EnumerateForwards(AttributeEnumFunc, &data);
    }

    AttributeSelectorEntry *entry = static_cast<AttributeSelectorEntry*>
                                               (PL_DHashTableOperate(&cascade->mAttributeSelectors, aData->mAttribute,
                             PL_DHASH_LOOKUP));
    if (PL_DHASH_ENTRY_IS_BUSY(entry)) {
      entry->mSelectors->EnumerateForwards(AttributeEnumFunc, &data);
    }
  }

  *aResult = data.change;
  return NS_OK;
}

NS_IMETHODIMP
nsCSSRuleProcessor::MediumFeaturesChanged(nsPresContext* aPresContext,
                                          PRBool* aRulesChanged)
{
  RuleCascadeData *old = mRuleCascades;
  // We don't want to do anything if there aren't any sets of rules
  // cached yet (or somebody cleared them and is thus responsible for
  // rebuilding things), since we should not build the rule cascade too
  // early (e.g., before we know whether the quirk style sheet should be
  // enabled).  And if there's nothing cached, it doesn't matter if
  // anything changed.  See bug 448281.
  if (old) {
    RefreshRuleCascade(aPresContext);
  }
  *aRulesChanged = (old != mRuleCascades);
  return NS_OK;
}

// Append all the currently-active font face rules to aArray.  Return
// true for success and false for failure.
PRBool
nsCSSRuleProcessor::AppendFontFaceRules(
                              nsPresContext *aPresContext,
                              nsTArray<nsFontFaceRuleContainer>& aArray)
{
  RuleCascadeData* cascade = GetRuleCascade(aPresContext);

  if (cascade) {
    if (!aArray.AppendElements(cascade->mFontFaceRules))
      return PR_FALSE;
  }
  
  return PR_TRUE;
}

nsresult
nsCSSRuleProcessor::ClearRuleCascades()
{
  // We rely on our caller (perhaps indirectly) to do something that
  // will rebuild style data and the user font set (either
  // nsIPresShell::ReconstructStyleData or
  // nsPresContext::RebuildAllStyleData).
  RuleCascadeData *data = mRuleCascades;
  mRuleCascades = nsnull;
  while (data) {
    RuleCascadeData *next = data->mNext;
    delete data;
    data = next;
  }
  return NS_OK;
}


// This function should return true only for selectors that need to be
// checked by |HasStateDependentStyle|.
inline
PRBool IsStateSelector(nsCSSSelector& aSelector)
{
  for (nsPseudoClassList* pseudoClass = aSelector.mPseudoClassList;
       pseudoClass; pseudoClass = pseudoClass->mNext) {
    if ((pseudoClass->mAtom == nsCSSPseudoClasses::active) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::checked) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::mozDragOver) || 
        (pseudoClass->mAtom == nsCSSPseudoClasses::focus) || 
        (pseudoClass->mAtom == nsCSSPseudoClasses::hover) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::target) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::link) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::visited) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::enabled) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::disabled) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::mozBroken) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::mozUserDisabled) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::mozSuppressed) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::mozLoading) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::required) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::optional) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::valid) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::invalid) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::inRange) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::outOfRange) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::mozReadOnly) ||
        (pseudoClass->mAtom == nsCSSPseudoClasses::mozReadWrite) ||
#ifdef MOZ_MATHML
        (pseudoClass->mAtom == nsCSSPseudoClasses::mozMathIncrementScriptLevel) ||
#endif
        (pseudoClass->mAtom == nsCSSPseudoClasses::defaultPseudo)) {
      return PR_TRUE;
    }
  }
  return PR_FALSE;
}

static PRBool
AddRule(RuleValue* aRuleInfo, void* aCascade)
{
  RuleCascadeData *cascade = static_cast<RuleCascadeData*>(aCascade);

  // Build the rule hash.
  cascade->mRuleHash.PrependRule(aRuleInfo);

  nsVoidArray* stateArray = &cascade->mStateSelectors;
  nsVoidArray* classArray = &cascade->mClassSelectors;
  nsVoidArray* idArray = &cascade->mIDSelectors;
  
  for (nsCSSSelector* selector = aRuleInfo->mSelector;
           selector; selector = selector->mNext) {
    // It's worth noting that this loop over negations isn't quite
    // optimal for two reasons.  One, we could add something to one of
    // these lists twice, which means we'll check it twice, but I don't
    // think that's worth worrying about.   (We do the same for multiple
    // attribute selectors on the same attribute.)  Two, we don't really
    // need to check negations past the first in the current
    // implementation (and they're rare as well), but that might change
    // in the future if :not() is extended. 
    for (nsCSSSelector* negation = selector; negation;
         negation = negation->mNegations) {
      // Build mStateSelectors.
      if (IsStateSelector(*negation))
        stateArray->AppendElement(selector);

      // Build mIDSelectors
      if (negation->mIDList) {
        idArray->AppendElement(selector);
      }
      
      // Build mClassSelectors
      if (negation->mClassList) {
        classArray->AppendElement(selector);
      }

      // Build mAttributeSelectors.
      for (nsAttrSelector *attr = negation->mAttrList; attr;
           attr = attr->mNext) {
        nsVoidArray *array = cascade->AttributeListFor(attr->mAttr);
        if (!array)
          return PR_FALSE;
        array->AppendElement(selector);
      }
    }
  }

  return PR_TRUE;
}

struct PerWeightData {
  PRInt32 mWeight;
  RuleValue* mRules; // linked list (reverse order)
};

struct RuleByWeightEntry : public PLDHashEntryHdr {
  PerWeightData data; // mWeight is key, mRules are value
};

static PLDHashNumber
HashIntKey(PLDHashTable *table, const void *key)
{
  return PLDHashNumber(NS_PTR_TO_INT32(key));
}

static PRBool
MatchWeightEntry(PLDHashTable *table, const PLDHashEntryHdr *hdr,
                 const void *key)
{
  const RuleByWeightEntry *entry = (const RuleByWeightEntry *)hdr;
  return entry->data.mWeight == NS_PTR_TO_INT32(key);
}

static PLDHashTableOps gRulesByWeightOps = {
    PL_DHashAllocTable,
    PL_DHashFreeTable,
    HashIntKey,
    MatchWeightEntry,
    PL_DHashMoveEntryStub,
    PL_DHashClearEntryStub,
    PL_DHashFinalizeStub,
    NULL
};

struct CascadeEnumData {
  CascadeEnumData(nsPresContext* aPresContext,
                  nsTArray<nsFontFaceRuleContainer>& aFontFaceRules,
                  nsMediaQueryResultCacheKey& aKey,
                  PLArenaPool& aArena,
                  PRUint8 aSheetType)
    : mPresContext(aPresContext),
      mFontFaceRules(aFontFaceRules),
      mCacheKey(aKey),
      mArena(aArena),
      mSheetType(aSheetType)
  {
    if (!PL_DHashTableInit(&mRulesByWeight, &gRulesByWeightOps, nsnull,
                          sizeof(RuleByWeightEntry), 64))
      mRulesByWeight.ops = nsnull;
  }

  ~CascadeEnumData()
  {
    if (mRulesByWeight.ops)
      PL_DHashTableFinish(&mRulesByWeight);
  }

  nsPresContext* mPresContext;
  nsTArray<nsFontFaceRuleContainer>& mFontFaceRules;
  nsMediaQueryResultCacheKey& mCacheKey;
  // Hooray, a manual PLDHashTable since nsClassHashtable doesn't
  // provide a getter that gives me a *reference* to the value.
  PLDHashTable mRulesByWeight; // of RuleValue* linked lists (?)
  PLArenaPool& mArena;
  PRUint8 mSheetType;
};

/*
 * This enumerates style rules in a sheet (and recursively into any
 * grouping rules) in order to:
 *  (1) add any style rules, in order, into data->mRulesByWeight (for
 *      the primary CSS cascade), where they are separated by weight
 *      but kept in order per-weight, and
 *  (2) add any @font-face rules, in order, into data->mFontFaceRules.
 */
static PRBool
CascadeRuleEnumFunc(nsICSSRule* aRule, void* aData)
{
  CascadeEnumData* data = (CascadeEnumData*)aData;
  PRInt32 type = nsICSSRule::UNKNOWN_RULE;
  aRule->GetType(type);

  if (nsICSSRule::STYLE_RULE == type) {
    nsICSSStyleRule* styleRule = (nsICSSStyleRule*)aRule;

    for (nsCSSSelectorList *sel = styleRule->Selector();
         sel; sel = sel->mNext) {
      PRInt32 weight = sel->mWeight;
      RuleByWeightEntry *entry = static_cast<RuleByWeightEntry*>(
        PL_DHashTableOperate(&data->mRulesByWeight, NS_INT32_TO_PTR(weight),
                             PL_DHASH_ADD));
      if (!entry)
        return PR_FALSE;
      entry->data.mWeight = weight;
      RuleValue *info =
        new (data->mArena) RuleValue(styleRule, sel->mSelectors);
      // entry->data.mRules must be in backwards order.
      info->mNext = entry->data.mRules;
      entry->data.mRules = info;
    }
  }
  else if (nsICSSRule::MEDIA_RULE == type ||
           nsICSSRule::DOCUMENT_RULE == type) {
    nsICSSGroupRule* groupRule = (nsICSSGroupRule*)aRule;
    if (groupRule->UseForPresentation(data->mPresContext, data->mCacheKey))
      if (!groupRule->EnumerateRulesForwards(CascadeRuleEnumFunc, aData))
        return PR_FALSE;
  }
  else if (nsICSSRule::FONT_FACE_RULE == type) {
    nsCSSFontFaceRule *fontFaceRule = static_cast<nsCSSFontFaceRule*>(aRule);
    nsFontFaceRuleContainer *ptr = data->mFontFaceRules.AppendElement();
    if (!ptr)
      return PR_FALSE;
    ptr->mRule = fontFaceRule;
    ptr->mSheetType = data->mSheetType;
  }

  return PR_TRUE;
}

/* static */ PRBool
nsCSSRuleProcessor::CascadeSheetEnumFunc(nsICSSStyleSheet* aSheet, void* aData)
{
  nsCSSStyleSheet*  sheet = static_cast<nsCSSStyleSheet*>(aSheet);
  CascadeEnumData* data = static_cast<CascadeEnumData*>(aData);
  PRBool bSheetApplicable = PR_TRUE;
  sheet->GetApplicable(bSheetApplicable);

  if (bSheetApplicable &&
      sheet->UseForPresentation(data->mPresContext, data->mCacheKey) &&
      sheet->mInner) {
    nsCSSStyleSheet* child = sheet->mInner->mFirstChild;
    while (child) {
      CascadeSheetEnumFunc(child, data);
      child = child->mNext;
    }

    if (!sheet->mInner->mOrderedRules.EnumerateForwards(CascadeRuleEnumFunc,
                                                        data))
      return PR_FALSE;
  }
  return PR_TRUE;
}

static int CompareWeightData(const void* aArg1, const void* aArg2,
                             void* closure)
{
  const PerWeightData* arg1 = static_cast<const PerWeightData*>(aArg1);
  const PerWeightData* arg2 = static_cast<const PerWeightData*>(aArg2);
  return arg1->mWeight - arg2->mWeight; // put lower weight first
}


struct FillWeightArrayData {
  FillWeightArrayData(PerWeightData* aArrayData) :
    mIndex(0),
    mWeightArray(aArrayData)
  {
  }
  PRInt32 mIndex;
  PerWeightData* mWeightArray;
};


static PLDHashOperator
FillWeightArray(PLDHashTable *table, PLDHashEntryHdr *hdr,
                PRUint32 number, void *arg)
{
  FillWeightArrayData* data = static_cast<FillWeightArrayData*>(arg);
  const RuleByWeightEntry *entry = (const RuleByWeightEntry *)hdr;

  data->mWeightArray[data->mIndex++] = entry->data;

  return PL_DHASH_NEXT;
}

RuleCascadeData*
nsCSSRuleProcessor::GetRuleCascade(nsPresContext* aPresContext)
{
  // If anything changes about the presentation context, we will be
  // notified.  Otherwise, our cache is valid if mLastPresContext
  // matches aPresContext.  (The only rule processors used for multiple
  // pres contexts are for XBL.  These rule processors are probably less
  // likely to have @media rules, and thus the cache is pretty likely to
  // hit instantly even when we're switching between pres contexts.)

  if (!mRuleCascades || aPresContext != mLastPresContext) {
    RefreshRuleCascade(aPresContext);
  }
  mLastPresContext = aPresContext;

  return mRuleCascades;
}

void
nsCSSRuleProcessor::RefreshRuleCascade(nsPresContext* aPresContext)
{
  // Having RuleCascadeData objects be per-medium (over all variation
  // caused by media queries, handled through mCacheKey) works for now
  // since nsCSSRuleProcessor objects are per-document.  (For a given
  // set of stylesheets they can vary based on medium (@media) or
  // document (@-moz-document).)

  for (RuleCascadeData **cascadep = &mRuleCascades, *cascade;
       (cascade = *cascadep); cascadep = &cascade->mNext) {
    if (cascade->mCacheKey.Matches(aPresContext)) {
      // Ensure that the current one is always mRuleCascades.
      *cascadep = cascade->mNext;
      cascade->mNext = mRuleCascades;
      mRuleCascades = cascade;

      return;
    }
  }

  if (mSheets.Count() != 0) {
    nsAutoPtr<RuleCascadeData> newCascade(
      new RuleCascadeData(aPresContext->Medium(),
                          eCompatibility_NavQuirks == aPresContext->CompatibilityMode()));
    if (newCascade) {
      CascadeEnumData data(aPresContext, newCascade->mFontFaceRules,
                           newCascade->mCacheKey,
                           newCascade->mRuleHash.Arena(),
                           mSheetType);
      if (!data.mRulesByWeight.ops)
        return; /* out of memory */
      if (!mSheets.EnumerateForwards(CascadeSheetEnumFunc, &data))
        return; /* out of memory */

      // Sort the hash table of per-weight linked lists by weight.
      PRUint32 weightCount = data.mRulesByWeight.entryCount;
      nsAutoArrayPtr<PerWeightData> weightArray(new PerWeightData[weightCount]);
      FillWeightArrayData fwData(weightArray);
      PL_DHashTableEnumerate(&data.mRulesByWeight, FillWeightArray, &fwData);
      NS_QuickSort(weightArray, weightCount, sizeof(PerWeightData),
                   CompareWeightData, nsnull);

      // Put things into the rule hash backwards because it's easier to
      // build a singly linked list lowest-first that way.
      // The primary sort is by weight...
      PRUint32 i = weightCount;
      while (i > 0) {
        --i;
        // and the secondary sort is by order.  mRules are already backwards.
        RuleValue *ruleValue = weightArray[i].mRules;
        do {
          // Calling |AddRule| reuses mNext!
          RuleValue *next = ruleValue->mNext;
          if (!AddRule(ruleValue, newCascade))
            return; /* out of memory */
          ruleValue = next;
        } while (ruleValue);
      }

      // Ensure that the current one is always mRuleCascades.
      newCascade->mNext = mRuleCascades;
      mRuleCascades = newCascade.forget();
    }
  }
  return;
}

/* static */ PRBool
nsCSSRuleProcessor::SelectorListMatches(RuleProcessorData& aData,
                                        nsCSSSelectorList* aSelectorList)
{
  while (aSelectorList) {
    nsCSSSelector* sel = aSelectorList->mSelectors;
    NS_ASSERTION(sel, "Should have *some* selectors");
    if (SelectorMatches(aData, sel, 0, nsnull, PR_FALSE)) {
      nsCSSSelector* next = sel->mNext;
      if (!next || SelectorMatchesTree(aData, next, PR_FALSE)) {
        return PR_TRUE;
      }
    }

    aSelectorList = aSelectorList->mNext;
  }

  return PR_FALSE;
}
