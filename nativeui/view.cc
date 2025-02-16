// Copyright 2016 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#include "nativeui/view.h"

#include <utility>

#include "base/strings/string_util.h"
#include "nativeui/container.h"
#include "nativeui/cursor.h"
#include "nativeui/gfx/font.h"
#include "nativeui/state.h"
#include "nativeui/util/yoga_util.h"
#include "nativeui/window.h"
#include "third_party/yoga/Yoga.h"

#if defined(OS_WIN)
#include "base/strings/utf_string_conversions.h"
#endif

// This header required DEBUG to be defined.
#if defined(DEBUG)
#include "third_party/yoga/YGNodePrint.h"
#else
#define DEBUG
#include "third_party/yoga/YGNodePrint.h"
#undef DEBUG
#endif

namespace nu {

namespace {

// Convert case to lower and remove non-ASCII characters.
std::string ParseName(const std::string& name) {
  std::string parsed;
  parsed.reserve(name.size());
  for (char c : name) {
    if (base::IsAsciiAlpha(c))
      parsed.push_back(base::ToLowerASCII(c));
  }
  return parsed;
}

}  // namespace

View::View() : view_(nullptr) {
  // Create node with the default yoga config.
  yoga_config_ = YGConfigNew();
  YGConfigCopy(yoga_config_, State::GetCurrent()->yoga_config());
  node_ = YGNodeNewWithConfig(yoga_config_);
  YGNodeSetContext(node_, this);
}

View::~View() {
  PlatformDestroy();

  // Free yoga config and node.
  YGNodeFree(node_);
  YGConfigFree(yoga_config_);
}

void View::SetVisible(bool visible) {
  if (visible == IsVisible())
    return;
  PlatformSetVisible(visible);
  YGNodeStyleSetDisplay(node_, visible ? YGDisplayFlex : YGDisplayNone);
  Layout();
}

void View::Layout() {
  // By default just make parent do layout.
  if (GetParent() && GetParent()->IsContainer())
    static_cast<Container*>(GetParent())->Layout();
}

int View::DoDrag(std::vector<Clipboard::Data> data, int operations) {
  DragOptions options;
  return DoDragWithOptions(std::move(data), operations, options);
}

void View::SetCursor(scoped_refptr<Cursor> cursor) {
  if (cursor_ == cursor)
    return;
  PlatformSetCursor(cursor.get());
  cursor_ = std::move(cursor);
}

void View::SetTooltip(std::string tooltip) {
#if defined(OS_MAC)
  PlatformSetTooltip(tooltip);
#elif defined(OS_LINUX) || defined(OS_WIN)
  Tooltip record = {
#if defined(OS_WIN)
      base::UTF8ToWide(tooltip),
#else
      std::move(tooltip),
#endif
      RectF()
  };
  PlatformSetTooltip(record.text);
  tooltips_.clear();
  tooltips_[default_tooltip_id_] = std::move(record);
#endif
}

int View::AddTooltipForRect(std::string tooltip, RectF rect) {
#if defined(OS_MAC)
  // On mac the ID is generated from API.
  return PlatformAddTooltipForRect(tooltip, rect);
#elif defined(OS_LINUX) || defined(OS_WIN)
  Tooltip record = {
#if defined(OS_WIN)
      base::UTF8ToWide(tooltip),
#else
      std::move(tooltip),
#endif
      RectF()
  };
  int id = PlatformAddTooltipForRect(record.text.c_str(), rect);
  tooltips_.erase(default_tooltip_id_);
  tooltips_[id] = std::move(record);
  return id;
#endif
}

void View::RemoveTooltip(int id) {
#if defined(OS_LINUX) || defined(OS_WIN)
  tooltips_.erase(id);
#endif
  PlatformRemoveTooltip(id);
}

void View::SetFont(scoped_refptr<Font> font) {
  if (font_ == font)
    return;
  PlatformSetFont(font.get());
  font_ = std::move(font);
  UpdateDefaultStyle();
}

void View::UpdateDefaultStyle() {
  SizeF min_size = GetMinimumSize();
  YGNodeStyleSetMinWidth(node_, min_size.width());
  YGNodeStyleSetMinHeight(node_, min_size.height());
  Layout();
}

void View::SetStyleProperty(const std::string& name, const std::string& value) {
  std::string key(ParseName(name));
  if (key == "color")
    SetColor(Color(value));
  else if (key == "backgroundcolor")
    SetBackgroundColor(Color(value));
  else
    SetYogaProperty(node_, key, value);
}

void View::SetStyleProperty(const std::string& name, float value) {
  SetYogaProperty(node_, ParseName(name), value);
}

std::string View::GetComputedLayout() const {
  std::string result;
  auto options = static_cast<YGPrintOptions>(YGPrintOptionsLayout |
                                             YGPrintOptionsStyle |
                                             YGPrintOptionsChildren);
  facebook::yoga::YGNodeToString(result, node_, options, 0);
  return result;
}

SizeF View::GetMinimumSize() const {
  return SizeF();
}

void View::SetParent(View* parent) {
  if (parent)
    YGConfigCopy(yoga_config_, parent->yoga_config_);
  parent_ = parent;
}

void View::BecomeContentView(Window* window) {
  if (window) {
    YGConfigCopy(yoga_config_, window->GetYogaConfig());
  }
  parent_ = nullptr;
}

bool View::IsContainer() const {
  return false;
}

void View::OnSizeChanged() {
  on_size_changed.Emit(this);
}

}  // namespace nu
