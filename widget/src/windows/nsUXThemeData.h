/* vim: se cin sw=2 ts=2 et : */
/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
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
 * The Original Code is the Mozilla browser.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1999
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Rob Arnold <robarnold@mozilla.com> (Original Author)
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
#include <windows.h>
#include "nscore.h"

#ifndef WM_DWMCOMPOSITIONCHANGED
#define WM_DWMCOMPOSITIONCHANGED        0x031E
#endif

enum nsUXThemeClass {
  eUXButton = 0,
  eUXEdit,
  eUXTooltip,
  eUXRebar,
  eUXMediaRebar,
  eUXCommunicationsRebar,
  eUXBrowserTabBarRebar,
  eUXToolbar,
  eUXMediaToolbar,
  eUXCommunicationsToolbar,
  eUXProgress,
  eUXTab,
  eUXScrollbar,
  eUXTrackbar,
  eUXSpin,
  eUXStatus,
  eUXCombobox,
  eUXHeader,
  eUXListview,
  eUXMenu,
  eUXNumClasses
};

struct MARGINS
{
  int cxLeftWidth;
  int cxRightWidth;
  int cyTopHeight;
  int cyBottomHeight;
};

class nsUXThemeData {
  static HMODULE sThemeDLL;
  static HMODULE sDwmDLL;
  static HANDLE sThemes[eUXNumClasses];
  
  static const wchar_t *GetClassName(nsUXThemeClass);

public:
  static const PRUnichar kThemeLibraryName[];
  static const PRUnichar kDwmLibraryName[];
  static BOOL sFlatMenus;
  static PRPackedBool sIsXPOrLater;
  static PRPackedBool sIsVistaOrLater;
  static PRPackedBool sHaveCompositor;
  static void Initialize();
  static void Teardown();
  static void Invalidate();
  static HANDLE GetTheme(nsUXThemeClass cls);
  static HMODULE GetThemeDLL();
  static HMODULE GetDwmDLL();

  static inline BOOL IsAppThemed() {
    return isAppThemed && isAppThemed();
  }

  static inline HRESULT GetThemeColor(nsUXThemeClass cls, int iPartId, int iStateId,
                                                   int iPropId, OUT COLORREF* pFont) {
    if(!getThemeColor)
      return E_FAIL;
    return getThemeColor(GetTheme(cls), iPartId, iStateId, iPropId, pFont);
  }

  // UXTheme.dll Function typedefs and declarations
  typedef HANDLE (WINAPI*OpenThemeDataPtr)(HWND hwnd, LPCWSTR pszClassList);
  typedef HRESULT (WINAPI*CloseThemeDataPtr)(HANDLE hTheme);
  typedef HRESULT (WINAPI*DrawThemeBackgroundPtr)(HANDLE hTheme, HDC hdc, int iPartId, 
                                            int iStateId, const RECT *pRect,
                                            const RECT* pClipRect);
  typedef HRESULT (WINAPI*DrawThemeEdgePtr)(HANDLE hTheme, HDC hdc, int iPartId, 
                                            int iStateId, const RECT *pDestRect,
                                            uint uEdge, uint uFlags,
                                            const RECT* pContentRect);
  typedef HRESULT (WINAPI*GetThemeContentRectPtr)(HANDLE hTheme, HDC hdc, int iPartId,
                                            int iStateId, const RECT* pRect,
                                            RECT* pContentRect);
  typedef HRESULT (WINAPI*GetThemePartSizePtr)(HANDLE hTheme, HDC hdc, int iPartId,
                                         int iStateId, RECT* prc, int ts,
                                         SIZE* psz);
  typedef HRESULT (WINAPI*GetThemeSysFontPtr)(HANDLE hTheme, int iFontId, OUT LOGFONT* pFont);
  typedef HRESULT (WINAPI*GetThemeColorPtr)(HANDLE hTheme, int iPartId,
                                     int iStateId, int iPropId, OUT COLORREF* pFont);
  typedef HRESULT (WINAPI*GetThemeMarginsPtr)(HANDLE hTheme, HDC hdc, int iPartId,
                                           int iStateid, int iPropId,
                                           LPRECT prc, MARGINS *pMargins);
  typedef BOOL (WINAPI*IsAppThemedPtr)(VOID);
  typedef HRESULT (WINAPI*GetCurrentThemeNamePtr)(LPWSTR pszThemeFileName, int dwMaxNameChars,
                                                  LPWSTR pszColorBuff, int cchMaxColorChars,
                                                  LPWSTR pszSizeBuff, int cchMaxSizeChars);
  typedef COLORREF (WINAPI*GetThemeSysColorPtr)(HANDLE hTheme, int iColorID);

  static OpenThemeDataPtr openTheme;
  static CloseThemeDataPtr closeTheme;
  static DrawThemeBackgroundPtr drawThemeBG;
  static DrawThemeEdgePtr drawThemeEdge;
  static GetThemeContentRectPtr getThemeContentRect;
  static GetThemePartSizePtr getThemePartSize;
  static GetThemeSysFontPtr getThemeSysFont;
  static GetThemeColorPtr getThemeColor;
  static GetThemeMarginsPtr getThemeMargins;
  static IsAppThemedPtr isAppThemed;
  static GetCurrentThemeNamePtr getCurrentThemeName;
  static GetThemeSysColorPtr getThemeSysColor;

  // dwmapi.dll function typedefs and declarations
  typedef HRESULT (WINAPI*DwmExtendFrameIntoClientAreaProc)(HWND hWnd, const MARGINS *pMarInset);
  typedef HRESULT (WINAPI*DwmIsCompositionEnabledProc)(BOOL *pfEnabled);

  static DwmExtendFrameIntoClientAreaProc dwmExtendFrameIntoClientAreaPtr;
  static DwmIsCompositionEnabledProc dwmIsCompositionEnabledPtr;

  static PRBool CheckForCompositor() {
    BOOL compositionIsEnabled = FALSE;
    if(dwmIsCompositionEnabledPtr)
      dwmIsCompositionEnabledPtr(&compositionIsEnabled);
    return sHaveCompositor = (compositionIsEnabled != 0);
  }
};
