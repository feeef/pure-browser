/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
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
 *   Neil Cronin (neil@rackle.com)
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

#include "nsScrollPortView.h"
#include "nsIWidget.h"
#include "nsIDeviceContext.h"
#include "nsGUIEvent.h"
#include "nsWidgetsCID.h"
#include "nsViewsCID.h"
#include "nsIScrollableView.h"
#include "nsILookAndFeel.h"
#include "nsISupportsArray.h"
#include "nsIScrollPositionListener.h"
#include "nsIRegion.h"
#include "nsViewManager.h"
#include "nsIPrefBranch.h"
#include "nsIPrefService.h"
#include "nsCOMPtr.h"
#include "nsServiceManagerUtils.h"

#include <math.h>

static NS_DEFINE_IID(kWidgetCID, NS_CHILD_CID);

#define SMOOTH_SCROLL_MSECS_PER_FRAME 10
#define SMOOTH_SCROLL_FRAMES    10

#define SMOOTH_SCROLL_PREF_NAME "general.smoothScroll"

class AsyncScroll {
public:
  AsyncScroll() {}
  ~AsyncScroll() {
    if (mScrollTimer) mScrollTimer->Cancel();
  }

  nsCOMPtr<nsITimer> mScrollTimer;
  PRInt32 mVelocities[SMOOTH_SCROLL_FRAMES*2];
  PRInt32 mFrameIndex;
  PRPackedBool mIsSmoothScroll;
};

nsScrollPortView::nsScrollPortView(nsViewManager* aViewManager)
  : nsView(aViewManager)
{
  mOffsetX = mOffsetY = 0;
  mDestinationX = mDestinationY = 0;
  nsCOMPtr<nsIDeviceContext> dev;
  mViewManager->GetDeviceContext(*getter_AddRefs(dev));
  mLineHeight = dev->AppUnitsPerInch() / 6; // 12 pt

  mListeners = nsnull;
  mAsyncScroll = nsnull;
}

nsScrollPortView::~nsScrollPortView()
{    
  if (nsnull != mListeners) {
    mListeners->Clear();
    NS_RELEASE(mListeners);
  }

  if (nsnull != mViewManager) {
     nsIScrollableView* scrollingView;
     mViewManager->GetRootScrollableView(&scrollingView);
     if ((nsnull != scrollingView) && (this == scrollingView)) {
       mViewManager->SetRootScrollableView(nsnull);
     }
  }

  delete mAsyncScroll;
}

nsresult nsScrollPortView::QueryInterface(const nsIID& aIID, void** aInstancePtr)
{
  if (nsnull == aInstancePtr) {
    return NS_ERROR_NULL_POINTER;
  }
  *aInstancePtr = nsnull;
  if (aIID.Equals(NS_GET_IID(nsIScrollableView))) {
    *aInstancePtr = (void*)(nsIScrollableView*)this;
    return NS_OK;
  }

  return nsView::QueryInterface(aIID, aInstancePtr);
}

NS_IMETHODIMP_(nsIView*) nsScrollPortView::View()
{
  return this;
}

NS_IMETHODIMP nsScrollPortView::AddScrollPositionListener(nsIScrollPositionListener* aListener)
{
  if (nsnull == mListeners) {
    nsresult rv = NS_NewISupportsArray(&mListeners);
    if (NS_FAILED(rv))
      return rv;
  }
  return mListeners->AppendElement(aListener);
}

NS_IMETHODIMP nsScrollPortView::RemoveScrollPositionListener(nsIScrollPositionListener* aListener)
{
  if (nsnull != mListeners) {
    return mListeners->RemoveElement(aListener);
  }
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP nsScrollPortView::CreateScrollControls(nsNativeWidget aNative)
{
  nsWidgetInitData  initData;
  initData.clipChildren = PR_TRUE;
  initData.clipSiblings = PR_TRUE;

  CreateWidget(kWidgetCID, &initData,
               mWindow ? nsnull : aNative);
  
  return NS_OK;
}

NS_IMETHODIMP nsScrollPortView::GetContainerSize(nscoord *aWidth, nscoord *aHeight) const
{
  if (!aWidth || !aHeight)
    return NS_ERROR_NULL_POINTER;

  *aWidth  = 0;
  *aHeight = 0;

  nsView *scrolledView = GetScrolledView();

  if (!scrolledView)
    return NS_ERROR_FAILURE;

  nsSize sz;
  scrolledView->GetDimensions(sz);
  *aWidth = sz.width;
  *aHeight = sz.height;
  return NS_OK;
}

static void ComputeVelocities(PRInt32 aCurVelocity, nscoord aCurPos, nscoord aDstPos,
                              PRInt32* aVelocities, PRInt32 aP2A) {
  // scrolling always works in units of whole pixels. So compute velocities
  // in pixels and then scale them up. This ensures, for example, that
  // a 1-pixel scroll isn't broken into N frames of 1/N pixels each, each
  // frame increment being rounded to 0 whole pixels.
  aCurPos = NSAppUnitsToIntPixels(aCurPos, aP2A);
  aDstPos = NSAppUnitsToIntPixels(aDstPos, aP2A);

  PRInt32 i;
  PRInt32 direction = (aCurPos < aDstPos ? 1 : -1);
  PRInt32 absDelta = (aDstPos - aCurPos)*direction;
  PRInt32 baseVelocity = absDelta/SMOOTH_SCROLL_FRAMES;

  for (i = 0; i < SMOOTH_SCROLL_FRAMES; i++) {
    aVelocities[i*2] = baseVelocity;
  }
  nscoord total = baseVelocity*SMOOTH_SCROLL_FRAMES;
  for (i = 0; i < SMOOTH_SCROLL_FRAMES; i++) {
    if (total < absDelta) {
      aVelocities[i*2]++;
      total++;
    }
  }
  NS_ASSERTION(total == absDelta, "Invalid velocity sum");

  PRInt32 scale = NSIntPixelsToAppUnits(direction, aP2A);
  for (i = 0; i < SMOOTH_SCROLL_FRAMES; i++) {
    aVelocities[i*2] *= scale;
  }
}
  
static nsresult ClampScrollValues(nscoord& aX, nscoord& aY, nsScrollPortView* aThis) {
  // make sure the new position in in bounds
  nsView* scrolledView = aThis->GetScrolledView();
  if (!scrolledView) return NS_ERROR_FAILURE;
  
  nsRect scrolledRect;
  scrolledView->GetDimensions(scrolledRect);
  
  nsSize portSize;
  aThis->GetDimensions(portSize);
  
  nscoord maxX = scrolledRect.XMost() - portSize.width;
  nscoord maxY = scrolledRect.YMost() - portSize.height;
  
  if (aX > maxX)
    aX = maxX;
  
  if (aY > maxY)
    aY = maxY;
  
  if (aX < scrolledRect.x)
    aX = scrolledRect.x;
  
  if (aY < scrolledRect.y)
    aY = scrolledRect.y;
  
  return NS_OK;
}
  
/*
 * this method wraps calls to ScrollToImpl(), either in one shot or incrementally,
 *  based on the setting of the smooth scroll pref
 */
NS_IMETHODIMP nsScrollPortView::ScrollTo(nscoord aDestinationX, nscoord aDestinationY,
                                         PRUint32 aUpdateFlags)
{
  mDestinationX = aDestinationX;
  mDestinationY = aDestinationY;
  ClampScrollValues(mDestinationX, mDestinationY, this);

  if (!(aUpdateFlags & (NS_VMREFRESH_DEFERRED | NS_VMREFRESH_SMOOTHSCROLL))) {
    // Asynchronous scrolling is not allowed, so we'll kill any existing
    // async-scrolling process and do an instant scroll
    delete mAsyncScroll;
    mAsyncScroll = nsnull;
    return ScrollToImpl(mDestinationX, mDestinationY);
  }

  PRInt32 currentVelocityX = 0;
  PRInt32 currentVelocityY = 0;
  PRBool isSmoothScroll = (aUpdateFlags & NS_VMREFRESH_SMOOTHSCROLL) &&
                          IsSmoothScrollingEnabled();

  if (mAsyncScroll) {
    if (mAsyncScroll->mIsSmoothScroll) {
      currentVelocityX = mAsyncScroll->mVelocities[mAsyncScroll->mFrameIndex*2];
      currentVelocityY = mAsyncScroll->mVelocities[mAsyncScroll->mFrameIndex*2 + 1];
    }
  } else {
    mAsyncScroll = new AsyncScroll;
    if (mAsyncScroll) {
      mAsyncScroll->mScrollTimer = do_CreateInstance("@mozilla.org/timer;1");
      if (!mAsyncScroll->mScrollTimer) {
        delete mAsyncScroll;
        mAsyncScroll = nsnull;
      }
    }
    if (!mAsyncScroll) {
      // some allocation failed. Scroll the normal way.
      return ScrollToImpl(mDestinationX, mDestinationY);
    }
    if (isSmoothScroll) {
      mAsyncScroll->mScrollTimer->InitWithFuncCallback(
        AsyncScrollCallback, this, SMOOTH_SCROLL_MSECS_PER_FRAME,
        nsITimer::TYPE_REPEATING_PRECISE);
    } else {
      mAsyncScroll->mScrollTimer->InitWithFuncCallback(
        AsyncScrollCallback, this, 0, nsITimer::TYPE_ONE_SHOT);
    }
  }

  mAsyncScroll->mFrameIndex = 0;
  mAsyncScroll->mIsSmoothScroll = isSmoothScroll;

  if (isSmoothScroll) {
    nsCOMPtr<nsIDeviceContext> dev;
    mViewManager->GetDeviceContext(*getter_AddRefs(dev));
    PRInt32 p2a = dev->AppUnitsPerDevPixel();

    // compute velocity vectors
    ComputeVelocities(currentVelocityX, mOffsetX, mDestinationX,
                      mAsyncScroll->mVelocities, p2a);
    ComputeVelocities(currentVelocityY, mOffsetY, mDestinationY,
                      mAsyncScroll->mVelocities + 1, p2a);
  }

  return NS_OK;
}

static void AdjustChildWidgets(nsView *aView,
  nsPoint aWidgetToParentViewOrigin, PRInt32 aP2A, PRBool aInvalidate)
{
  if (aView->HasWidget()) {
    nsIWidget* widget = aView->GetWidget();
    nsWindowType type;
    widget->GetWindowType(type);
    if (type != eWindowType_popup) {
      nsRect bounds = aView->GetBounds();
      nsPoint widgetOrigin = aWidgetToParentViewOrigin
        + nsPoint(bounds.x, bounds.y);
      widget->Move(NSAppUnitsToIntPixels(widgetOrigin.x, aP2A),
                   NSAppUnitsToIntPixels(widgetOrigin.y, aP2A));
      if (aInvalidate) {
        // Force the widget and everything in it to repaint. We can't
        // just use Invalidate because the widget might have child
        // widgets and they wouldn't get updated. We can't call
        // UpdateView(aView) because the area to be repainted might be
        // outside aView's clipped bounds. This isn't the greatest way
        // to achieve this, perhaps, but it works.
        widget->Show(PR_FALSE);
        widget->Show(PR_TRUE);
      }
    }
  } else {
    // Don't recurse if the view haLs a widget, because we adjusted the view's
    // widget position, and its child widgets are relative to its positon
    nsPoint widgetToViewOrigin = aWidgetToParentViewOrigin
      + aView->GetPosition();

    for (nsView* kid = aView->GetFirstChild(); kid; kid = kid->GetNextSibling())
    {
      AdjustChildWidgets(kid, widgetToViewOrigin, aP2A, aInvalidate);
    }
  }
}


NS_IMETHODIMP nsScrollPortView::SetScrolledView(nsIView *aScrolledView)
{
  NS_ASSERTION(GetFirstChild() == nsnull || GetFirstChild()->GetNextSibling() == nsnull,
               "Error scroll port has too many children");

  // if there is already a child so remove it
  if (GetFirstChild() != nsnull)
  {
    mViewManager->RemoveChild(GetFirstChild());
  }

  return mViewManager->InsertChild(this, aScrolledView, 0);
}

NS_IMETHODIMP nsScrollPortView::GetScrolledView(nsIView *&aScrolledView) const
{
  aScrolledView = GetScrolledView();
  return NS_OK;
}

NS_IMETHODIMP nsScrollPortView::GetScrollPosition(nscoord &aX, nscoord &aY) const
{
  aX = mOffsetX;
  aY = mOffsetY;

  return NS_OK;
}

NS_IMETHODIMP nsScrollPortView::SetScrollProperties(PRUint32 aProperties)
{
  mScrollProperties = aProperties;
  return NS_OK;
}

NS_IMETHODIMP nsScrollPortView::GetScrollProperties(PRUint32 *aProperties)
{
  *aProperties = mScrollProperties;
  return NS_OK;
}

NS_IMETHODIMP nsScrollPortView::SetLineHeight(nscoord aHeight)
{
  mLineHeight = aHeight;
  return NS_OK;
}

NS_IMETHODIMP nsScrollPortView::GetLineHeight(nscoord *aHeight)
{
  *aHeight = mLineHeight;
  return NS_OK;
}

NS_IMETHODIMP nsScrollPortView::ScrollByLines(PRInt32 aNumLinesX, PRInt32 aNumLinesY,
                                              PRUint32 aUpdateFlags)
{
  nscoord dx = mLineHeight*aNumLinesX;
  nscoord dy = mLineHeight*aNumLinesY;

  return ScrollTo(mDestinationX + dx, mDestinationY + dy, aUpdateFlags);
}

NS_IMETHODIMP nsScrollPortView::GetPageScrollDistances(nsSize *aDistances)
{
  nsSize size;
  GetDimensions(size);

  // The page increment is the size of the page, minus the smaller of
  // 10% of the size or 2 lines.
  aDistances->width  = size.width  - PR_MIN(size.width  / 10, 2 * mLineHeight);
  aDistances->height = size.height - PR_MIN(size.height / 10, 2 * mLineHeight);

  return NS_OK;
}

NS_IMETHODIMP nsScrollPortView::ScrollByPages(PRInt32 aNumPagesX, PRInt32 aNumPagesY,
                                              PRUint32 aUpdateFlags)
{
  nsSize delta;
  GetPageScrollDistances(&delta);
    
  // put in the number of pages.
  delta.width *= aNumPagesX;
  delta.height *= aNumPagesY;

  return ScrollTo(mDestinationX + delta.width, mDestinationY + delta.height,
                  aUpdateFlags);
}

NS_IMETHODIMP nsScrollPortView::ScrollByWhole(PRBool aTop,
                                              PRUint32 aUpdateFlags)
{
  nscoord   newPos = 0;

  if (!aTop) {
    nsSize scrolledSize;
    nsView* scrolledView = GetScrolledView();
    scrolledView->GetDimensions(scrolledSize);
    newPos = scrolledSize.height;
  }

  ScrollTo(mDestinationX, newPos, aUpdateFlags);

  return NS_OK;
}

NS_IMETHODIMP nsScrollPortView::ScrollByPixels(PRInt32 aNumPixelsX,
                                               PRInt32 aNumPixelsY,
                                               PRUint32 aUpdateFlags)
{
  nsCOMPtr<nsIDeviceContext> dev;
  mViewManager->GetDeviceContext(*getter_AddRefs(dev));
  PRInt32 p2a = dev->AppUnitsPerDevPixel(); 

  nscoord dx = NSIntPixelsToAppUnits(aNumPixelsX, p2a);
  nscoord dy = NSIntPixelsToAppUnits(aNumPixelsY, p2a);

  return ScrollTo(mDestinationX + dx, mDestinationY + dy, aUpdateFlags);
}

NS_IMETHODIMP nsScrollPortView::CanScroll(PRBool aHorizontal,
                                          PRBool aForward,
                                          PRBool &aResult)
{
  nscoord offset = aHorizontal ? mOffsetX : mOffsetY;

  nsView* scrolledView = GetScrolledView();
  if (!scrolledView) {
    aResult = PR_FALSE;
    return NS_ERROR_FAILURE;
  }

  nsRect scrolledRect;
  scrolledView->GetDimensions(scrolledRect);

  // Can scroll to Top or to Left?
  if (!aForward) {
    aResult = offset > (aHorizontal ? scrolledRect.x : scrolledRect.y);
    return NS_OK;
  }

  nsSize portSize;
  GetDimensions(portSize);

  nsCOMPtr<nsIDeviceContext> dev;
  mViewManager->GetDeviceContext(*getter_AddRefs(dev));
  PRInt32 p2a = dev->AppUnitsPerDevPixel();

  nscoord max;
  if (aHorizontal) {
    max = scrolledRect.XMost() - portSize.width;
    // Round by pixel
    nscoord maxPx = NSAppUnitsToIntPixels(max, p2a);
    max = NSIntPixelsToAppUnits(maxPx, p2a);
  } else {
    max = scrolledRect.YMost() - portSize.height;
    // Round by pixel
    nscoord maxPx = NSAppUnitsToIntPixels(max, p2a);
    max = NSIntPixelsToAppUnits(maxPx, p2a);
  }

  // Can scroll to Bottom or to Right?
  aResult = (offset < max) ? PR_TRUE : PR_FALSE;

  return NS_OK;
}

void nsScrollPortView::Scroll(nsView *aScrolledView, nsPoint aTwipsDelta, nsPoint aPixDelta,
                              PRInt32 aP2A)
{
  if (aTwipsDelta.x != 0 || aTwipsDelta.y != 0)
  {
    /* If we should invalidate our wrapped view, we should do so at this
     * point.
     */
    if (aScrolledView->NeedsInvalidateFrameOnScroll())
      GetViewManager()->GetViewObserver()->InvalidateFrameForView(aScrolledView);
    
    nsIWidget *scrollWidget = GetWidget();
    nsRegion updateRegion;
    PRBool canBitBlit = scrollWidget &&
                        ((mScrollProperties & NS_SCROLL_PROPERTY_ALWAYS_BLIT) ||
                         mViewManager->CanScrollWithBitBlt(aScrolledView, aTwipsDelta, &updateRegion)) &&
                        scrollWidget->GetTransparencyMode() != eTransparencyTransparent;

    if (canBitBlit) {
      // We're going to bit-blit.  Let the viewmanager know so it can
      // adjust dirty regions appropriately.
      mViewManager->WillBitBlit(this, aTwipsDelta);
    }
    
    if (!scrollWidget)
    {
      NS_ASSERTION(!canBitBlit, "Someone screwed up");
      nsPoint offsetToWidget;
      GetNearestWidget(&offsetToWidget);
      // We're moving the child widgets because we are scrolling. But
      // the child widgets may stick outside our bounds, so their area
      // may include area that's not supposed to be scrolled. We need
      // to invalidate to ensure that any such area is properly
      // repainted back to the right rendering.
      AdjustChildWidgets(aScrolledView, offsetToWidget, aP2A, PR_TRUE);
      // If we don't have a scroll widget then we must just update.
      // We should call this after fixing up the widget positions to be
      // consistent with the view hierarchy.
      mViewManager->UpdateView(this, NS_VMREFRESH_DEFERRED);
    } else if (!canBitBlit) {
      // We can't blit for some reason.
      // Just update the view and adjust widgets
      // Recall that our widget's origin is at our bounds' top-left
      nsRect bounds(GetBounds());
      nsPoint topLeft(bounds.x, bounds.y);
      AdjustChildWidgets(aScrolledView,
                         GetPosition() - topLeft, aP2A, PR_FALSE);
      // We should call this after fixing up the widget positions to be
      // consistent with the view hierarchy.
      mViewManager->UpdateView(this, NS_VMREFRESH_DEFERRED);
    } else { // if we can blit and have a scrollwidget then scroll.
      nsRect* toScrollPtr = nsnull;

#ifdef XP_WIN
      nsRect toScroll;
      if (!updateRegion.IsEmpty()) {
        nsRegion regionToScroll;
        regionToScroll.Sub(nsRect(nsPoint(0,0), GetBounds().Size()),
                           updateRegion);
        nsRegionRectIterator iter(regionToScroll);
        nsRect biggestRect(0,0,0,0);
        const nsRect* r;
        for (r = iter.Next(); r; r = iter.Next()) {
          if (r->width*r->height > biggestRect.width*biggestRect.height) {
            biggestRect = *r;
          }
        }
        toScrollPtr = &toScroll;
        biggestRect.ScaleRoundIn(1.0/aP2A);
        toScroll = biggestRect;
        biggestRect *= aP2A;
        regionToScroll.Sub(regionToScroll, biggestRect);
        updateRegion.Or(updateRegion, regionToScroll);
      }
#endif

      // Scroll the contents of the widget by the specified amount, and scroll
      // the child widgets
      scrollWidget->Scroll(aPixDelta.x, aPixDelta.y, toScrollPtr);
      mViewManager->UpdateViewAfterScroll(this, updateRegion);
    }
  }
}

NS_IMETHODIMP nsScrollPortView::ScrollToImpl(nscoord aX, nscoord aY)
{
  PRInt32           dxPx = 0, dyPx = 0;

  // convert to pixels
  nsCOMPtr<nsIDeviceContext> dev;
  mViewManager->GetDeviceContext(*getter_AddRefs(dev));
  PRInt32 p2a = dev->AppUnitsPerDevPixel();

  // Update the scrolled view's position
  nsresult rv = ClampScrollValues(aX, aY, this);
  if (NS_FAILED(rv)) {
    return rv;
  }
  
  PRInt32 xPixels = NSAppUnitsToIntPixels(aX, p2a);
  PRInt32 yPixels = NSAppUnitsToIntPixels(aY, p2a);
  
  aX = NSIntPixelsToAppUnits(xPixels, p2a);
  aY = NSIntPixelsToAppUnits(yPixels, p2a);
  
  // do nothing if the we aren't scrolling.
  // this needs to be rechecked because of the clamping and
  // rounding
  if (aX == mOffsetX && aY == mOffsetY) {
    return NS_OK;
  }

  // figure out the diff by comparing old pos to new
  dxPx = NSAppUnitsToIntPixels(mOffsetX, p2a) - xPixels;
  dyPx = NSAppUnitsToIntPixels(mOffsetY, p2a) - yPixels;

  // notify the listeners.
  PRUint32 listenerCount;
  const nsIID& kScrollPositionListenerIID = NS_GET_IID(nsIScrollPositionListener);
  nsIScrollPositionListener* listener;
  if (nsnull != mListeners) {
    if (NS_SUCCEEDED(mListeners->Count(&listenerCount))) {
      for (PRUint32 i = 0; i < listenerCount; i++) {
        if (NS_SUCCEEDED(mListeners->QueryElementAt(i, kScrollPositionListenerIID, (void**)&listener))) {
          listener->ScrollPositionWillChange(this, aX, aY);
          NS_RELEASE(listener);
        }
      }
    }
  }
  
  nsView* scrolledView = GetScrolledView();
  if (!scrolledView) return NS_ERROR_FAILURE;

  // move the scrolled view to the new location
  // Note that child widgets may be scrolled by the native widget scrolling,
  // so don't update their positions
  scrolledView->SetPositionIgnoringChildWidgets(-aX, -aY);
      
  // notify the listeners.
  if (nsnull != mListeners) {
    if (NS_SUCCEEDED(mListeners->Count(&listenerCount))) {
      for (PRUint32 i = 0; i < listenerCount; i++) {
        if (NS_SUCCEEDED(mListeners->QueryElementAt(i, kScrollPositionListenerIID, (void**)&listener))) {
          listener->ViewPositionDidChange(this);
          NS_RELEASE(listener);
        }
      }
    }
  }

  nsPoint twipsDelta(aX - mOffsetX, aY - mOffsetY);

  // store the new position
  mOffsetX = aX;
  mOffsetY = aY;

  Scroll(scrolledView, twipsDelta, nsPoint(dxPx, dyPx), p2a);

  mViewManager->SynthesizeMouseMove(PR_TRUE);
  
  // notify the listeners.
  if (nsnull != mListeners) {
    if (NS_SUCCEEDED(mListeners->Count(&listenerCount))) {
      for (PRUint32 i = 0; i < listenerCount; i++) {
        if (NS_SUCCEEDED(mListeners->QueryElementAt(i, kScrollPositionListenerIID, (void**)&listener))) {
          listener->ScrollPositionDidChange(this, aX, aY);
          NS_RELEASE(listener);
        }
      }
    }
  }
 
  return NS_OK;
}

PRBool nsScrollPortView::IsSmoothScrollingEnabled() {
  nsCOMPtr<nsIPrefBranch> prefs = do_GetService(NS_PREFSERVICE_CONTRACTID);
  if (prefs) {
    PRBool enabled;
    nsresult rv = prefs->GetBoolPref(SMOOTH_SCROLL_PREF_NAME, &enabled);
    if (NS_SUCCEEDED(rv)) {
      return enabled;
    }
  }
  return PR_FALSE;
}

/*
 * Callback function from timer used in nsScrollPortView::ScrollTo
 */
void
nsScrollPortView::AsyncScrollCallback(nsITimer *aTimer, void* anInstance) 
{
  nsScrollPortView* self = static_cast<nsScrollPortView*>(anInstance);
  if (self) {
    self->IncrementalScroll();
  }
}

/*
 * manages data members and calls to ScrollTo from the (static) AsyncScrollCallback method
 */ 
void
nsScrollPortView::IncrementalScroll()
{
  if (!mAsyncScroll)
    return;

  nsWeakView thisView = this;
  if (mAsyncScroll->mIsSmoothScroll) {
    if (mAsyncScroll->mFrameIndex < SMOOTH_SCROLL_FRAMES) {
      ScrollToImpl(mOffsetX + mAsyncScroll->mVelocities[mAsyncScroll->mFrameIndex*2],
                   mOffsetY + mAsyncScroll->mVelocities[mAsyncScroll->mFrameIndex*2 + 1]);
      if (!thisView.IsAlive())
        return;
      // A nested ScrollTo() taking the synchronous path may have deleted
      // |mAsyncScroll| so we need to null-check again.  Bug 490461.
      if (mAsyncScroll)
        mAsyncScroll->mFrameIndex++;
      return;
    }
  } else {
    ScrollToImpl(mDestinationX, mDestinationY);
    if (!thisView.IsAlive())
      return;
  }
  delete mAsyncScroll;
  mAsyncScroll = nsnull;
}
