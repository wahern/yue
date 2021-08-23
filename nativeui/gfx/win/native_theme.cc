// Copyright 2016 Cheng Zhao. All rights reserved.
// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#include "nativeui/gfx/win/native_theme.h"

#include <stddef.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <vssym32.h>

#include "base/check_op.h"
#include "base/cxx17_backports.h"
#include "base/notreached.h"
#include "base/win/registry.h"
#include "base/win/win_util.h"
#include "base/win/windows_version.h"

namespace nu {

namespace {

int GetWindowsPart(NativeTheme::Part part) {
  if (part == NativeTheme::Part::Checkbox)
    return BP_CHECKBOX;
  else if (part == NativeTheme::Part::Radio)
    return BP_RADIOBUTTON;
  else if (part == NativeTheme::Part::Button)
    return BP_PUSHBUTTON;
  else
    return 0;
}

int GetWindowsState(NativeTheme::Part part, ControlState state) {
  switch (part) {
    case NativeTheme::Part::Checkbox:
      switch (state) {
        case ControlState::Disabled:
          return CBS_UNCHECKEDDISABLED;
        case ControlState::Hovered:
          return CBS_UNCHECKEDHOT;
        case ControlState::Normal:
          return CBS_UNCHECKEDNORMAL;
        case ControlState::Pressed:
          return CBS_UNCHECKEDPRESSED;
        case ControlState::Size:
          NOTREACHED();
          return 0;
      }
    case NativeTheme::Part::Button:
      switch (state) {
        case ControlState::Disabled:
          return PBS_DISABLED;
        case ControlState::Hovered:
          return PBS_HOT;
        case ControlState::Normal:
          return PBS_DEFAULTED;
        case ControlState::Pressed:
          return PBS_PRESSED;
        case ControlState::Size:
          NOTREACHED();
          return 0;
      }
    case NativeTheme::Part::Radio:
      switch (state) {
        case ControlState::Disabled:
          return RBS_UNCHECKEDDISABLED;
        case ControlState::Hovered:
          return RBS_UNCHECKEDHOT;
        case ControlState::Normal:
          return RBS_UNCHECKEDNORMAL;
        case ControlState::Pressed:
          return RBS_UNCHECKEDPRESSED;
        case ControlState::Size:
          NOTREACHED();
          return 0;
      }
    default:
      return 0;
  }
}

bool IsHighContrast() {
  HIGHCONTRASTW high_contrast = {sizeof(high_contrast)};
  if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(high_contrast),
                            &high_contrast, FALSE))
    return high_contrast.dwFlags & HCF_HIGHCONTRASTON;
  return false;
}

}  // namespace

NativeTheme::NativeTheme() {
  memset(theme_handles_, 0, sizeof(theme_handles_));
}

NativeTheme::~NativeTheme() {
  if (theme_dll_) {
    CloseHandles();
    FreeLibrary(theme_dll_);
  }
}

bool NativeTheme::InitializeDarkMode() {
  if (!dark_mode_supported_) {
    theme_dll_ = LoadLibrary(L"uxtheme.dll");
    DCHECK(theme_dll_);

    const auto* os_info = base::win::OSInfo::GetInstance();
    const auto version = os_info->version();
    if ((version >= base::win::Version::WIN10_RS5) &&
        (version <= base::win::Version::WIN10_20H1)) {
      open_nc_theme_date_ = reinterpret_cast<OpenNcThemeDataPtr>(
          GetProcAddress(theme_dll_, MAKEINTRESOURCEA(49)));
      should_app_use_dark_mode_ = reinterpret_cast<ShouldAppsUseDarkModePtr>(
          GetProcAddress(theme_dll_, MAKEINTRESOURCEA(132)));
      allow_dark_mode_for_window_ = reinterpret_cast<AllowDarkModeForWindowPtr>(
          GetProcAddress(theme_dll_, MAKEINTRESOURCEA(133)));
      if (os_info->version_number().build < 18362)
        allow_dark_mode_for_app_ = reinterpret_cast<AllowDarkModeForAppPtr>(
            GetProcAddress(theme_dll_, MAKEINTRESOURCEA(135)));
      else
        set_preferred_app_mode_ = reinterpret_cast<SetPreferredAppModePtr>(
            GetProcAddress(theme_dll_, MAKEINTRESOURCEA(135)));
      refresh_color_policy_ =
          reinterpret_cast<RefreshImmersiveColorPolicyStatePtr>(
              GetProcAddress(theme_dll_, MAKEINTRESOURCEA(104)));
      set_window_attribute_ =
          reinterpret_cast<SetWindowCompositionAttributePtr>(
              base::win::GetUser32FunctionPointer(
                  "SetWindowCompositionAttribute"));

      dark_mode_supported_ =
          open_nc_theme_date_ &&
          should_app_use_dark_mode_ &&
          allow_dark_mode_for_window_ &&
          (allow_dark_mode_for_app_ || set_preferred_app_mode_) &&
          refresh_color_policy_;
    }
  }
  return *dark_mode_supported_;
}

bool NativeTheme::IsDarkModeSupported() const {
  return dark_mode_supported_.value_or(false);
}

void NativeTheme::SetAppDarkModeEnabled(bool enable) {
  if (!IsDarkModeSupported())
    return;
  if (allow_dark_mode_for_app_)
    allow_dark_mode_for_app_(enable);
  else if (set_preferred_app_mode_)
    set_preferred_app_mode_(enable ? AllowDark : Default);
  refresh_color_policy_();
}

void NativeTheme::EnableDarkModeForWindow(HWND hwnd) {
  if (!IsDarkModeSupported())
    return;
  allow_dark_mode_for_window_(hwnd, true);
  const auto* os_info = base::win::OSInfo::GetInstance();
  BOOL dark = TRUE;
  if (os_info->version_number().build < 18362) {
    ::SetPropW(hwnd, L"UseImmersiveDarkModeColors",
               reinterpret_cast<HANDLE>(static_cast<INT_PTR>(dark)));
  } else if (set_window_attribute_) {
    WINDOWCOMPOSITIONATTRIBDATA data = {
      WCA_USEDARKMODECOLORS, &dark, sizeof(dark)
    };
    set_window_attribute_(hwnd, &data);
  }
}

bool NativeTheme::IsAppDarkMode() const {
  if (!IsDarkModeSupported())
    return false;
  return should_app_use_dark_mode_() && !IsHighContrast();
}

bool NativeTheme::IsSystemDarkMode() const {
  base::win::RegKey hkcu_themes_regkey;
  hkcu_themes_regkey.Open(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\"
                          L"Themes\\Personalize",
                          KEY_READ);
  if (!hkcu_themes_regkey.Valid())
    return false;
  DWORD apps_use_light_theme = 1;
  hkcu_themes_regkey.ReadValueDW(L"AppsUseLightTheme",
                                 &apps_use_light_theme);
  return apps_use_light_theme == 0;
}

Size NativeTheme::GetThemePartSize(HDC hdc,
                                   Part part,
                                   ControlState state) const {
  HANDLE handle = GetThemeHandle(part);
  if (handle) {
    SIZE size;
    int part_id = GetWindowsPart(part);
    int state_id = GetWindowsState(part, state);
    if (SUCCEEDED(::GetThemePartSize(handle, hdc, part_id, state_id,
                                     nullptr, TS_TRUE, &size)))
      return Size(size.cx, size.cy);
  }

  return (part == Part::Checkbox || part == Part::Radio) ? Size(13, 13)
                                                         : Size();
}

void NativeTheme::Paint(Part part, HDC hdc, ControlState state,
                        const Rect& rect, const ExtraParams& extra) {
  switch (part) {
    case Part::Checkbox:
      PaintCheckbox(hdc, state, rect, extra.button);
      break;
    case Part::Radio:
      PaintRadio(hdc, state, rect, extra.button);
      break;
    case Part::Button:
      PaintPushButton(hdc, state, rect, extra.button);
      break;
    case Part::ScrollbarUpArrow:
    case Part::ScrollbarDownArrow:
    case Part::ScrollbarLeftArrow:
    case Part::ScrollbarRightArrow:
      PaintScrollbarArrow(part, hdc, state, rect, extra.scrollbar_arrow);
      break;
    case Part::ScrollbarHorizontalThumb:
    case Part::ScrollbarVerticalThumb:
    case Part::ScrollbarHorizontalGripper:
    case Part::ScrollbarVerticalGripper:
      PaintScrollbarThumb(part, hdc, state, rect, extra.scrollbar_thumb);
      break;
    case Part::ScrollbarHorizontalTrack:
    case Part::ScrollbarVerticalTrack:
      PaintScrollbarTrack(part, hdc, state, rect, extra.scrollbar_track);
      break;
    case Part::TabPanel:
      PaintTabPanel(part, hdc, state, rect);
      break;
    case Part::TabItem:
      PaintTabItem(part, hdc, state, rect);
      break;
    default:
      NOTREACHED();
  }
}

HRESULT NativeTheme::PaintPushButton(HDC hdc,
                                     ControlState state,
                                     const Rect& rect,
                                     const ButtonExtraParams& extra) const {
  int state_id = extra.is_default ? PBS_DEFAULTED : PBS_NORMAL;
  switch (state) {
    case ControlState::Disabled:
      state_id = PBS_DISABLED;
      break;
    case ControlState::Hovered:
      state_id = PBS_HOT;
      break;
    case ControlState::Normal:
      break;
    case ControlState::Pressed:
      state_id = PBS_PRESSED;
      break;
    case ControlState::Size:
      NOTREACHED();
      break;
  }

  RECT rect_win = rect.ToRECT();
  return PaintButton(hdc, state, extra, BP_PUSHBUTTON, state_id, &rect_win);
}

HRESULT NativeTheme::PaintRadio(HDC hdc,
                                ControlState state,
                                const Rect& rect,
                                const ButtonExtraParams& extra) const {
  int state_id = extra.checked ? RBS_CHECKEDNORMAL : RBS_UNCHECKEDNORMAL;
  switch (state) {
    case ControlState::Disabled:
      state_id = extra.checked ? RBS_CHECKEDDISABLED : RBS_UNCHECKEDDISABLED;
      break;
    case ControlState::Hovered:
      state_id = extra.checked ? RBS_CHECKEDHOT : RBS_UNCHECKEDHOT;
      break;
    case ControlState::Normal:
      break;
    case ControlState::Pressed:
      state_id = extra.checked ? RBS_CHECKEDPRESSED : RBS_UNCHECKEDPRESSED;
      break;
    case ControlState::Size:
      NOTREACHED();
      break;
  }

  RECT rect_win = rect.ToRECT();
  return PaintButton(hdc, state, extra, BP_RADIOBUTTON, state_id, &rect_win);
}

HRESULT NativeTheme::PaintCheckbox(HDC hdc,
                                   ControlState state,
                                   const Rect& rect,
                                   const ButtonExtraParams& extra) const {
  int state_id = extra.checked ?
      CBS_CHECKEDNORMAL :
      (extra.indeterminate ? CBS_MIXEDNORMAL : CBS_UNCHECKEDNORMAL);
  switch (state) {
    case ControlState::Disabled:
      state_id = extra.checked ?
          CBS_CHECKEDDISABLED :
          (extra.indeterminate ? CBS_MIXEDDISABLED : CBS_UNCHECKEDDISABLED);
      break;
    case ControlState::Hovered:
      state_id = extra.checked ?
          CBS_CHECKEDHOT :
          (extra.indeterminate ? CBS_MIXEDHOT : CBS_UNCHECKEDHOT);
      break;
    case ControlState::Normal:
      break;
    case ControlState::Pressed:
      state_id = extra.checked ?
          CBS_CHECKEDPRESSED :
          (extra.indeterminate ? CBS_MIXEDPRESSED : CBS_UNCHECKEDPRESSED);
      break;
    case ControlState::Size:
      NOTREACHED();
      break;
  }

  RECT rect_win = rect.ToRECT();
  return PaintButton(hdc, state, extra, BP_CHECKBOX, state_id, &rect_win);
}

HRESULT NativeTheme::PaintScrollbarArrow(
    Part part,
    HDC hdc,
    ControlState state,
    const Rect& rect,
    const ScrollbarArrowExtraParams& extra) const {
  static const int
      state_id_matrix[4][static_cast<size_t>(ControlState::Size)] = {
      {ABS_UPDISABLED, ABS_UPHOT, ABS_UPNORMAL, ABS_UPPRESSED},
      {ABS_DOWNDISABLED, ABS_DOWNHOT, ABS_DOWNNORMAL, ABS_DOWNPRESSED},
      {ABS_LEFTDISABLED, ABS_LEFTHOT, ABS_LEFTNORMAL, ABS_LEFTPRESSED},
      {ABS_RIGHTDISABLED, ABS_RIGHTHOT, ABS_RIGHTNORMAL, ABS_RIGHTPRESSED},
  };
  HANDLE handle = GetThemeHandle(part);
  RECT rect_win = rect.ToRECT();
  if (handle) {
    int index =
        static_cast<int>(part) - static_cast<int>(Part::ScrollbarUpArrow);
    DCHECK_GE(index, 0);
    DCHECK_LT(static_cast<size_t>(index), base::size(state_id_matrix));
    int state_id = state_id_matrix[index][static_cast<int>(state)];

    // Hovering means that the cursor is over the scrollbar, but not over the
    // specific arrow itself.
    if (state == ControlState::Normal && extra.is_hovering) {
      switch (part) {
        case Part::ScrollbarDownArrow:
          state_id = ABS_DOWNHOVER;
          break;
        case Part::ScrollbarLeftArrow:
          state_id = ABS_LEFTHOVER;
          break;
        case Part::ScrollbarRightArrow:
          state_id = ABS_RIGHTHOVER;
          break;
        case Part::ScrollbarUpArrow:
          state_id = ABS_UPHOVER;
          break;
        default:
          NOTREACHED();
          break;
      }
    }
    return PaintScaledTheme(handle, hdc, SBP_ARROWBTN, state_id, rect);
  }

  int classic_state = DFCS_SCROLLDOWN;
  switch (part) {
    case Part::ScrollbarDownArrow:
      break;
    case Part::ScrollbarLeftArrow:
      classic_state = DFCS_SCROLLLEFT;
      break;
    case Part::ScrollbarRightArrow:
      classic_state = DFCS_SCROLLRIGHT;
      break;
    case Part::ScrollbarUpArrow:
      classic_state = DFCS_SCROLLUP;
      break;
    default:
      NOTREACHED();
      break;
  }
  switch (state) {
    case ControlState::Disabled:
      classic_state |= DFCS_INACTIVE;
      break;
    case ControlState::Hovered:
      classic_state |= DFCS_HOT;
      break;
    case ControlState::Normal:
      break;
    case ControlState::Pressed:
      classic_state |= DFCS_PUSHED;
      break;
    case ControlState::Size:
      NOTREACHED();
      break;
  }
  DrawFrameControl(hdc, &rect_win, DFC_SCROLL, classic_state);
  return S_OK;
}

HRESULT NativeTheme::PaintScrollbarThumb(
    Part part,
    HDC hdc,
    ControlState state,
    const Rect& rect,
    const ScrollbarThumbExtraParams& extra) const {
  HANDLE handle = GetThemeHandle(part);
  RECT rect_win = rect.ToRECT();

  int part_id = SBP_THUMBBTNVERT;
  switch (part) {
    case Part::ScrollbarHorizontalThumb:
      part_id = SBP_THUMBBTNHORZ;
      break;
    case Part::ScrollbarVerticalThumb:
      break;
    case Part::ScrollbarHorizontalGripper:
      part_id = SBP_GRIPPERHORZ;
      break;
    case Part::ScrollbarVerticalGripper:
      part_id = SBP_GRIPPERVERT;
      break;
    default:
      NOTREACHED();
      break;
  }

  int state_id = SCRBS_NORMAL;
  switch (state) {
    case ControlState::Disabled:
      state_id = SCRBS_DISABLED;
      break;
    case ControlState::Hovered:
      state_id = extra.is_hovering ? SCRBS_HOT : SCRBS_HOVER;
      break;
    case ControlState::Normal:
      state_id = extra.is_hovering ? SCRBS_HOVER : SCRBS_NORMAL;
      break;
    case ControlState::Pressed:
      state_id = SCRBS_PRESSED;
      break;
    case ControlState::Size:
      NOTREACHED();
      break;
  }

  if (handle)
    return PaintScaledTheme(handle, hdc, part_id, state_id, rect);

  // Draw it manually.
  if ((part_id == SBP_THUMBBTNHORZ) || (part_id == SBP_THUMBBTNVERT))
    DrawEdge(hdc, &rect_win, EDGE_RAISED, BF_RECT | BF_MIDDLE);
  // Classic mode doesn't have a gripper.
  return S_OK;
}

HRESULT NativeTheme::PaintScrollbarTrack(
    Part part,
    HDC hdc,
    ControlState state,
    const Rect& rect,
    const ScrollbarTrackExtraParams& extra) const {
  HANDLE handle = GetThemeHandle(part);
  RECT rect_win = rect.ToRECT();

  const int part_id = extra.is_upper ?
      ((part == Part::ScrollbarHorizontalTrack) ?
          SBP_UPPERTRACKHORZ : SBP_UPPERTRACKVERT) :
      ((part == Part::ScrollbarHorizontalTrack) ?
          SBP_LOWERTRACKHORZ : SBP_LOWERTRACKVERT);

  int state_id = SCRBS_NORMAL;
  switch (state) {
    case ControlState::Disabled:
      state_id = SCRBS_DISABLED;
      break;
    case ControlState::Hovered:
      state_id = SCRBS_HOVER;
      break;
    case ControlState::Normal:
      break;
    case ControlState::Pressed:
      state_id = SCRBS_PRESSED;
      break;
    case ControlState::Size:
      NOTREACHED();
      break;
  }

  if (handle)
    return DrawThemeBackground(handle, hdc, part_id, state_id, &rect_win, NULL);

  // Draw it manually.
  FillRect(hdc, &rect_win, reinterpret_cast<HBRUSH>(COLOR_SCROLLBAR + 1));
  if (state == ControlState::Pressed)
    InvertRect(hdc, &rect_win);
  return S_OK;
}

HRESULT NativeTheme::PaintTabPanel(Part part,
                                   HDC hdc,
                                   ControlState state,
                                   const Rect& rect) const {
  HANDLE handle = GetThemeHandle(part);
  RECT rect_win = rect.ToRECT();

  if (handle)
    return DrawThemeBackground(handle, hdc, TABP_PANE, 0, &rect_win, NULL);

  // TODO(zcbenz): Add fallback when visual theme is not enabled.
  return S_OK;
}

HRESULT NativeTheme::PaintTabItem(Part part,
                                  HDC hdc,
                                  ControlState state,
                                  const Rect& rect) const {
  static const int state_id_matrix[static_cast<size_t>(ControlState::Size)] = {
      TIS_DISABLED, TIS_HOT, TIS_NORMAL, TIS_SELECTED
  };

  HANDLE handle = GetThemeHandle(part);
  RECT rect_win = rect.ToRECT();

  const int part_id = TABP_TABITEM;
  int state_id = state_id_matrix[static_cast<int>(state)];

  if (handle)
    return DrawThemeBackground(handle, hdc, part_id, state_id, &rect_win, NULL);

  // TODO(zcbenz): Add fallback when visual theme is not enabled.
  return S_OK;
}

HRESULT NativeTheme::PaintButton(HDC hdc,
                                 ControlState state,
                                 const ButtonExtraParams& extra,
                                 int part_id,
                                 int state_id,
                                 RECT* rect) const {
  HANDLE handle = GetThemeHandle(Part::Button);
  if (handle)
    return DrawThemeBackground(handle, hdc, part_id, state_id, rect, NULL);

  // Adjust classic_state based on part, state, and extras.
  int classic_state = 0;
  switch (part_id) {
    case BP_CHECKBOX:
      classic_state |= DFCS_BUTTONCHECK;
      break;
    case BP_RADIOBUTTON:
      classic_state |= DFCS_BUTTONRADIO;
      break;
    case BP_PUSHBUTTON:
      classic_state |= DFCS_BUTTONPUSH;
      break;
    default:
      NOTREACHED();
      break;
  }

  switch (state) {
    case ControlState::Disabled:
      classic_state |= DFCS_INACTIVE;
      break;
    case ControlState::Hovered:
    case ControlState::Normal:
      break;
    case ControlState::Pressed:
      classic_state |= DFCS_PUSHED;
      break;
    case ControlState::Size:
      NOTREACHED();
      break;
  }

  if (extra.checked)
    classic_state |= DFCS_CHECKED;

  // Draw it manually.
  // All pressed states have both low bits set, and no other states do.
  const bool focused = ((state_id & ETS_FOCUSED) == ETS_FOCUSED);
  const bool pressed = ((state_id & PBS_PRESSED) == PBS_PRESSED);
  if ((BP_PUSHBUTTON == part_id) && (pressed || focused)) {
    // BP_PUSHBUTTON has a focus rect drawn around the outer edge, and the
    // button itself is shrunk by 1 pixel.
    HBRUSH brush = GetSysColorBrush(COLOR_3DDKSHADOW);
    if (brush) {
      FrameRect(hdc, rect, brush);
      InflateRect(rect, -1, -1);
    }
  }
  DrawFrameControl(hdc, rect, DFC_BUTTON, classic_state);

  // Classic theme doesn't support indeterminate checkboxes.  We draw
  // a recangle inside a checkbox like IE10 does.
  if (part_id == BP_CHECKBOX && extra.indeterminate) {
    RECT inner_rect = *rect;
    // "4 / 13" is same as IE10 in classic theme.
    int padding = (inner_rect.right - inner_rect.left) * 4 / 13;
    InflateRect(&inner_rect, -padding, -padding);
    int color_index = state == ControlState::Disabled ? COLOR_GRAYTEXT :
                                                        COLOR_WINDOWTEXT;
    FillRect(hdc, &inner_rect, GetSysColorBrush(color_index));
  }

  return S_OK;
}

HRESULT NativeTheme::PaintScaledTheme(HANDLE theme,
                                      HDC hdc,
                                      int part_id,
                                      int state_id,
                                      const Rect& rect) const {
  // Correct the scaling and positioning of sub-components such as scrollbar
  // arrows and thumb grippers in the event that the world transform applies
  // scaling (e.g. in high-DPI mode).
  XFORM save_transform;
  if (GetWorldTransform(hdc, &save_transform)) {
    float scale = save_transform.eM11;
    if (scale != 1 && save_transform.eM12 == 0) {
      ModifyWorldTransform(hdc, NULL, MWT_IDENTITY);
      Rect scaled_rect = ScaleToEnclosedRect(rect, scale);
      scaled_rect.Offset(save_transform.eDx, save_transform.eDy);
      RECT bounds = scaled_rect.ToRECT();
      HRESULT result = ::DrawThemeBackground(
          theme, hdc, part_id, state_id, &bounds, NULL);
      SetWorldTransform(hdc, &save_transform);
      return result;
    }
  }
  RECT bounds = rect.ToRECT();
  return ::DrawThemeBackground(theme, hdc, part_id, state_id, &bounds, NULL);
}

HANDLE NativeTheme::GetThemeHandle(Part part) const {
  // Translate part to real theme names.
  switch (part) {
    case Part::Checkbox:
    case Part::Radio:
      part = Part::Button;
      break;
    case Part::ScrollbarDownArrow:
    case Part::ScrollbarLeftArrow:
    case Part::ScrollbarRightArrow:
    case Part::ScrollbarUpArrow:
    case Part::ScrollbarHorizontalThumb:
    case Part::ScrollbarVerticalThumb:
    case Part::ScrollbarHorizontalTrack:
    case Part::ScrollbarVerticalTrack:
      part = Part::ScrollbarDownArrow;
      break;
    case Part::TabItem:
      part = Part::TabPanel;
    default:
      break;
  }

  if (theme_handles_[static_cast<int>(part)])
    return theme_handles_[static_cast<int>(part)];

  // Dark mode is not ready for most controls.
  const bool dark_mode = false;

  // Not found, try to load it.
  HANDLE handle = 0;
  switch (part) {
    case Part::Button:
      handle = dark_mode ? open_nc_theme_date_(NULL, L"Explorer::Button")
                         : ::OpenThemeData(NULL, L"Button");
      break;
    case Part::ScrollbarDownArrow:
      handle = dark_mode ? open_nc_theme_date_(NULL, L"Explorer::Scrollbar")
                         : ::OpenThemeData(NULL, L"Scrollbar");
      break;
    case Part::TabPanel:
      handle = ::OpenThemeData(NULL, L"Tab");
      break;
    default:
      NOTREACHED();
      break;
  }
  theme_handles_[static_cast<int>(part)] = handle;
  return handle;
}

void NativeTheme::CloseHandles() const {
  for (int i = 0; i < static_cast<int>(Part::Count); ++i) {
    if (theme_handles_[i]) {
      ::CloseThemeData(theme_handles_[i]);
      theme_handles_[i] = nullptr;
    }
  }
}

}  // namespace nu
