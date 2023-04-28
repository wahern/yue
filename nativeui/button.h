// Copyright 2016 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#ifndef NATIVEUI_BUTTON_H_
#define NATIVEUI_BUTTON_H_

#include <string>

#include "nativeui/view.h"

namespace nu {

class Image;

class NATIVEUI_EXPORT Button : public View {
 public:
  enum class Type {
#if defined(OS_MAC)
    // Following values should match NSButtonType.
    MomentaryLight = 0,
    PushOnPushOff,
    Toggle,
    Switch,
    Radio,
    MomentaryChange,
    OnOff,
    MomentaryPushIn,
    Accelerator, // API_AVAILABLE(macos(10.10.3)) = 8,
    MultiLevelAccelerator, // API_AVAILABLE(macos(10.10.3)) = 9,

    // Portable API types (may be treated specially by constructor).
    Normal = MomentaryPushIn,
    Checkbox = Switch,
    Disclosure = OnOff,
#else
    Normal,
    Checkbox,
    Radio,
#endif
  };
  explicit Button(const std::string& title, Type type = Type::Normal);

  // View class name.
  static const char kClassName[];

  void MakeDefault();
  void SetTitle(const std::string& title);
  std::string GetTitle() const;
  void SetChecked(bool checked);
  bool IsChecked() const;
  void SetImage(scoped_refptr<Image> image);
  Image* GetImage() const { return image_.get(); }

#if defined(OS_MAC)
  // Values here should match NSBezelStyle.
  enum class Style {
    Rounded = 1,
    RegularSquare,
    ThickSquare,
    ThickerSquare,
    Disclosure,
    ShadowlessSquare,
    Circular,
    TexturedSquare,
    HelpButton,
    SmallSquare,
    TexturedRounded,
    RoundRect,
    Recessed,
    RoundedDisclosure,
    Inline,
  };
  void SetButtonStyle(Style style);
  void SetButtonType(Type type);

  void SetHasBorder(bool yes);
  bool HasBorder() const;
#endif

  // View:
  const char* GetClassName() const override;
  SizeF GetMinimumSize() const override;

  // Events.
  Signal<void(Button*)> on_click;

 private:
  ~Button() override;

  void PlatformSetImage(Image* image);
  void PlatformSetTitle(const std::string& title);

  scoped_refptr<Image> image_;
};

}  // namespace nu

#endif  // NATIVEUI_BUTTON_H_
