// Copyright 2016 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#include "nativeui/view.h"

#include <gtk/gtk.h>

#include "base/strings/stringprintf.h"
#include "nativeui/container.h"
#include "nativeui/cursor.h"
#include "nativeui/gfx/font.h"
#include "nativeui/gfx/geometry/point_f.h"
#include "nativeui/gfx/geometry/rect_conversions.h"
#include "nativeui/gfx/geometry/rect_f.h"
#include "nativeui/gtk/dragging_info_gtk.h"
#include "nativeui/gtk/nu_container.h"
#include "nativeui/gtk/util/clipboard_util.h"
#include "nativeui/gtk/util/widget_util.h"
#include "nativeui/window.h"

namespace nu {

namespace {

// View private data.
struct NUViewPrivate {
  View* delegate;
  // Current view size.
  Size size;

  // The current drop session (dest).
  GdkDragContext* drop_context = nullptr;
  // The registerd accepted dragged types for the view.
  std::set<Clipboard::Data::Type> accepted_types;
  // The last drop operation, used for replying drag status.
  int last_drop_operation = -1;
  // The final operation of drop.
  int final_drop_operation = -1;
  // Received drag data.
  std::map<Clipboard::Data::Type, Clipboard::Data> received_data;

  // The current drag session (source).
  GdkDragContext* drag_context = nullptr;
  // The received operation of drag.
  int drag_operation = -1;
  // The drag data.
  std::vector<Clipboard::Data> drag_data;
};

// Helper to set cursor for view.
void NUSetCursor(GtkWidget* widget, GdkCursor* cursor) {
  GdkWindow* window = NU_IS_CONTAINER(widget) ?
      nu_container_get_window(NU_CONTAINER(widget)) :
      gtk_widget_get_window(widget);
  if (window)
    gdk_window_set_cursor(window, cursor);
}

// Some views are implemented by wrapping the actuall view inside a scroll
// view, and we must operate on the actuall view.
GtkWidget* GetTargetView(GtkWidget* view) {
  if (!GTK_IS_SCROLLED_WINDOW(view))
    return view;
  void* data = g_object_get_data(G_OBJECT(view), "widget");
  if (!data)
    return view;
  return GTK_WIDGET(data);
}

void OnSizeAllocate(GtkWidget* widget, GdkRectangle* allocation,
                    NUViewPrivate* priv) {
  // Ignore empty sizes on initialization.
  if (allocation->x == -1 && allocation->y == -1 &&
      allocation->width == 1 && allocation->height == 1)
    return;

  // Size allocation happens unnecessarily often.
  Size size(allocation->width, allocation->height);
  if (size != priv->size) {
    priv->size = size;
    priv->delegate->OnSizeChanged();
  }
}

void OnRealize(GtkWidget* widget, View* view) {
  if (view->cursor())
    NUSetCursor(widget, view->cursor()->GetNative());
}

void OnDragEnd(GtkWidget*, GdkDragContext*, NUViewPrivate* priv) {
  if (priv->drag_context) {
    priv->drag_operation =
        gdk_drag_context_get_suggested_action(priv->drag_context);
    priv->drag_context = nullptr;
    gtk_main_quit();
  }
}

void OnDragDataGet(GtkWidget* widget, GdkDragContext* context,
                   GtkSelectionData* selection, guint info, guint time,
                   NUViewPrivate* priv) {
  DCHECK_LT(info, priv->drag_data.size());
  FillSelection(selection, priv->drag_data[info]);
}

bool OnDragMotion(GtkWidget* widget, GdkDragContext* context,
                  gint x, gint y, guint time, NUViewPrivate* priv) {
  // Check if type is registerd.
  if (gtk_drag_dest_find_target(widget, context, nullptr) == GDK_NONE)
    return false;

  int r;
  View* view = priv->delegate;
  DraggingInfoGtk dragging_info(context);
  PointF point(x, y);
  if (priv->last_drop_operation == -1) {
    // This is the first motion.
    priv->drop_context = context;
    if (!view->handle_drag_enter)
      return false;
    r = view->handle_drag_enter(view, &dragging_info, point);
  } else {
    if (view->handle_drag_update)
      r = view->handle_drag_update(view, &dragging_info, point);
    else
      r = priv->last_drop_operation;
  }

  priv->last_drop_operation = r;
  if (r == DRAG_OPERATION_NONE)
    return false;
  gdk_drag_status(context, static_cast<GdkDragAction>(r), time);
  return true;
}

void OnDragLeave(GtkWidget* widget, GdkDragContext* context, guint time,
                 NUViewPrivate* priv) {
  priv->final_drop_operation = priv->last_drop_operation;
  priv->last_drop_operation = -1;

  View* view = priv->delegate;
  if (view->on_drag_leave.IsEmpty())
    return;
  DraggingInfoGtk dragging_info(context);
  view->on_drag_leave.Emit(view, &dragging_info);
}

bool OnDragDrop(GtkWidget* widget, GdkDragContext* context,
                gint x, gint y, guint time, NUViewPrivate* priv) {
  // This is the last step of drop, request data and wait.
  for (auto type : priv->accepted_types)
    gtk_drag_get_data(widget, context, GetAtomForType(type), time);
  return true;
}

void OnDragDataReceived(GtkWidget* widget, GdkDragContext* context,
                        gint x, gint y, GtkSelectionData* selection,
                        guint info, guint time, NUViewPrivate* priv) {
  // Do nothing if receiving data from old context.
  if (priv->drop_context != context)
    return;

  // Don't continue until all data have been received.
  auto type = static_cast<Clipboard::Data::Type>(info);
  priv->received_data[type] = GetDataFromSelection(selection, type);
  if (priv->received_data.size() < priv->accepted_types.size())
    return;

  // End of session.
  priv->drop_context = nullptr;

  // Emit events.
  View* view = priv->delegate;
  if (gtk_drag_dest_find_target(widget, context, nullptr) != GDK_NONE &&
      view->handle_drop) {
    DraggingInfoGtk dragging_info(context, std::move(priv->received_data));
    if (view->handle_drop(view, &dragging_info, PointF(x, y))) {
      gtk_drag_finish(context, true,
                      priv->final_drop_operation & GDK_ACTION_MOVE, time);
      return;
    }
  }

  // Clear and fail.
  priv->received_data.clear();
  gtk_drag_finish(context, false, false, time);
}

// Callback called by the query-tooltip signal.
gboolean OnQueryTooltip(GtkWidget*, gint x, gint y, gboolean keyboad_mode,
                        GtkTooltip* tooltip, View* view) {
  if (keyboad_mode)  // x and y are not defined
    return FALSE;
  return view->QueryTooltip(x, y, tooltip);
}

}  // namespace

void View::PlatformDestroy() {
  if (view_) {
    gtk_widget_destroy(view_);
    g_object_unref(view_);
    // The PlatformDestroy might be called for multiple times, see
    // Container::PlatformDestroy for more about this.
    view_ = nullptr;
  }
}

void View::TakeOverView(NativeView view) {
  InitResponder(view_ = view, Type::View);
  g_object_ref_sink(view);
  gtk_widget_show(view);  // visible by default

  NUViewPrivate* priv = new NUViewPrivate;
  priv->delegate = this;
  g_object_set_data_full(G_OBJECT(view), "private", priv,
                         Delete<NUViewPrivate>);

  // Make the view accepts events.
  gtk_widget_add_events(view, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                              GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);

  // Install event hooks.
  g_signal_connect(view, "size-allocate", G_CALLBACK(OnSizeAllocate), priv);
  g_signal_connect(view, "realize", G_CALLBACK(OnRealize), this);
}

Vector2dF View::OffsetFromView(const View* from) const {
  GdkRectangle rect_f;
  gtk_widget_get_allocation(from->GetNative(), &rect_f);
  GdkRectangle rect_d;
  gtk_widget_get_allocation(GetNative(), &rect_d);
  return Vector2dF(rect_d.x - rect_f.x, rect_d.y - rect_f.y);
}

Vector2dF View::OffsetFromWindow() const {
  Window* window = GetWindow();
  if (!window)
    return Vector2dF();
  return OffsetFromView(window->GetContentView()) -
         window->ContentBoundsToWindowBounds(
             window->GetContentView()->GetBounds()).OffsetFromOrigin();
}

void View::SetBounds(const RectF& bounds) {
  return SetPixelBounds(ToNearestRect(bounds));
}

RectF View::GetBounds() const {
  return RectF(GetPixelBounds());
}

RectF View::GetBoundsInScreen() const {
  if (!GetWindow())
    return GetBounds();
  // If the widget has a window, then get the position of window directly.
  GdkWindow* window = nullptr;
  if (NU_IS_CONTAINER(view_))
    window = nu_container_get_window(NU_CONTAINER(view_));
  else if (gtk_widget_get_has_window(view_))
    window = gtk_widget_get_window(view_);
  if (window) {
    gint x, y, width, height;
    gdk_window_get_origin(window, &x, &y);
    gdk_window_get_geometry(window, NULL, NULL, &width, &height);
    return RectF(x, y, width, height);
  }
  // Otherwise fallback to manual computing, they shouldn't make a difference
  // but we want to use raw APIs when possible for correctness.
  return nu::RectF(GetBounds().size()) +
         OffsetFromWindow() +
         GetWindow()->GetBounds().OffsetFromOrigin();
}

void View::SetPixelBounds(const Rect& bounds) {
  // The size allocation is relative to the window instead of parent.
  GdkRectangle rect = bounds.ToGdkRectangle();
  if (GetParent()) {
    GdkRectangle pb;
    gtk_widget_get_allocation(GetParent()->GetNative(), &pb);
    rect.x += pb.x;
    rect.y += pb.y;
  }

  // Call get_preferred_width before size allocation, otherwise GTK would print
  // warnings like "How does the code know the size to allocate?".
  gint tmp;
  gtk_widget_get_preferred_width(view_, &tmp, nullptr);
  gtk_widget_get_preferred_height(view_, &tmp, nullptr);

  gtk_widget_size_allocate(view_, &rect);
}

Rect View::GetPixelBounds() const {
  GdkRectangle rect;
  gtk_widget_get_allocation(view_, &rect);
  if (GetParent()) {
    // The size allocation is relative to the window instead of parent.
    GdkRectangle pb;
    gtk_widget_get_allocation(GetParent()->GetNative(), &pb);
    rect.x -= pb.x;
    rect.y -= pb.y;
  }
  Rect bounds = Rect(rect);
  // GTK uses (-1, -1, 1, 1) and (0, 0, 1, 1) as empty bounds, we should match
  // the behavior of other platforms by returning an empty rect.
  if (bounds == Rect(-1, -1, 1, 1) || bounds == Rect(0, 0, 1, 1))
    return Rect();
  // For content view, we need to remove menubar height.
  if (GetWindow() && GetWindow()->GetContentView() == this)
    bounds.set_y(0);
  return bounds;
}

void View::SchedulePaint() {
  gtk_widget_queue_draw(view_);
}

void View::SchedulePaintRect(const RectF& rect) {
  gtk_widget_queue_draw_area(view_,
                             rect.x(), rect.y(), rect.width(), rect.height());
}

void View::PlatformSetVisible(bool visible) {
  gtk_widget_set_visible(view_, visible);
}

bool View::IsVisible() const {
  return gtk_widget_get_visible(view_);
}

bool View::IsVisibleInHierarchy() const {
  // The gtk_widget_is_visible API returns false if the window is not visible,
  // while we don't want to consider window in our API.
  const View* view = this;
  while (view) {
    if (!view->IsVisible())
      return false;
    view = view->GetParent();
  }
  return true;
}

void View::SetEnabled(bool enable) {
  // Do not support disabling a container, to match other platforms' behavior.
  if (GTK_IS_CONTAINER(view_) && !GTK_IS_BIN(view_))
    return;
  gtk_widget_set_sensitive(view_, enable);
}

bool View::IsEnabled() const {
  return gtk_widget_get_sensitive(view_);
}

void View::Focus() {
  gtk_widget_grab_focus(GetTargetView(view_));
}

bool View::HasFocus() const {
  return gtk_widget_is_focus(GetTargetView(view_));
}

void View::SetFocusable(bool focusable) {
  gtk_widget_set_can_focus(view_, focusable);
}

bool View::IsFocusable() const {
  return gtk_widget_get_can_focus(view_);
}

void View::SetMouseDownCanMoveWindow(bool yes) {
  g_object_set_data(G_OBJECT(view_), "draggable", yes ? this : nullptr);
}

bool View::IsMouseDownCanMoveWindow() const {
  return g_object_get_data(G_OBJECT(view_), "draggable");
}

int View::DoDragWithOptions(std::vector<Clipboard::Data> objects,
                            int operations,
                            const DragOptions& options) {
  auto* priv = static_cast<NUViewPrivate*>(
      g_object_get_data(G_OBJECT(view_), "private"));
  if (priv->drag_context)
    return DRAG_OPERATION_NONE;

  GtkTargetList* targets = gtk_target_list_new(0, 0);
  for (size_t i = 0; i < objects.size(); ++i)
    FillTargetList(targets, objects[i].type(), i);

  priv->drag_data = std::move(objects);
  priv->drag_context = gtk_drag_begin_with_coordinates(
      view_, targets, static_cast<GdkDragAction>(operations), 1,
      nullptr, -1, -1);

  // Provide drag image if available.
  if (options.image)
    gtk_drag_set_icon_pixbuf(
        priv->drag_context,
        gdk_pixbuf_animation_get_static_image(options.image->GetNative()),
        0, 0);

  // Block until the drag operation is done.
  gtk_main();

  gtk_target_list_unref(targets);
  priv->drag_data.clear();
  return priv->drag_operation;
}

void View::CancelDrag() {
  auto* priv = static_cast<NUViewPrivate*>(
      g_object_get_data(G_OBJECT(view_), "private"));
  if (priv->drag_context)
    gtk_drag_cancel(priv->drag_context);
}

bool View::IsDragging() const {
  auto* priv = static_cast<NUViewPrivate*>(
      g_object_get_data(G_OBJECT(view_), "private"));
  return priv->drag_context;
}

void View::RegisterDraggedTypes(std::set<Clipboard::Data::Type> types) {
  auto* priv = static_cast<NUViewPrivate*>(
      g_object_get_data(G_OBJECT(view_), "private"));
  priv->accepted_types = std::move(types);

  auto defaults = static_cast<GtkDestDefaults>(0);
  if (priv->accepted_types.empty()) {
    gtk_drag_dest_set(view_, defaults, nullptr, 0, GDK_ACTION_DEFAULT);
    return;
  }

  GtkTargetList* targets = gtk_target_list_new(0, 0);
  for (auto type : priv->accepted_types)
    FillTargetList(targets, type, static_cast<int>(type));

  int size = 0;
  if (GtkTargetEntry* table = gtk_target_table_new_from_list(targets, &size)) {
    auto action = static_cast<GdkDragAction>(GDK_ACTION_COPY |
                                             GDK_ACTION_MOVE |
                                             GDK_ACTION_LINK);
    gtk_drag_dest_set(view_, defaults, table, size, action);
    gtk_target_table_free(table, size);
  }
  gtk_target_list_unref(targets);

  // Install drag drop event handlers.
  if (!on_drop_installed_) {
    g_signal_connect(view_, "drag-end", G_CALLBACK(OnDragEnd), priv);
    g_signal_connect(view_, "drag-data-get", G_CALLBACK(OnDragDataGet), priv);
    g_signal_connect(view_, "drag-motion", G_CALLBACK(OnDragMotion), priv);
    g_signal_connect(view_, "drag-leave", G_CALLBACK(OnDragLeave), priv);
    g_signal_connect(view_, "drag-drop", G_CALLBACK(OnDragDrop), priv);
    g_signal_connect(view_, "drag-data-received",
                     G_CALLBACK(OnDragDataReceived), priv);
    on_drop_installed_ = true;
  }
}

void View::PlatformSetCursor(Cursor* cursor) {
  if (!gtk_widget_get_has_window(view_) && !IsContainer())
    gtk_widget_set_has_window(view_, true);
  NUSetCursor(view_, cursor ? cursor->GetNative(): nullptr);
}

void View::PlatformSetTooltip(const std::string& tooltip) {
  // We own the tooltip string passed to the gtk.
  gtk_widget_set_tooltip_text(view_, tooltip.c_str());
}

int View::PlatformAddTooltipForRect(const std::string& tooltip, RectF rect) {
  // Connect to signal on demand.
  if (tooltip_signal_ == 0) {
    gtk_widget_set_has_tooltip(view_, true);
    tooltip_signal_ = g_signal_connect(view_, "query-tooltip",
                                       G_CALLBACK(OnQueryTooltip), this);
  }
  return ++next_tooltip_id_;
}

void View::PlatformRemoveTooltip(int id) {
  // Disconnet signal when there are no more tooltips.
  if (tooltips_.empty() && tooltip_signal_) {
    gtk_widget_set_has_tooltip(view_, false);
    g_signal_handler_disconnect(view_, tooltip_signal_);
    tooltip_signal_ = 0;
  }
}

void View::PlatformSetFont(Font* font) {
  gtk_widget_override_font(view_, font->GetNative());
}

void View::SetColor(Color color) {
  ApplyStyle(view_, "color",
             base::StringPrintf("* { color: %s; }",
                                color.ToString().c_str()));
}

void View::SetBackgroundColor(Color color) {
  ApplyStyle(view_, "background-color",
             base::StringPrintf("* { background-color: %s; }",
                                color.ToString().c_str()));
}

Window* View::GetWindow() const {
  GtkWidget* toplevel = gtk_widget_get_toplevel(view_);
  if (!gtk_widget_is_toplevel(toplevel))
    return nullptr;
  return Window::FromNative(GTK_WINDOW(toplevel));
}

bool View::QueryTooltip(int x, int y, GtkTooltip* tooltip) {
  for (const auto& t : tooltips_) {
    if (t.second.rect.Contains(x, y)) {
      gtk_tooltip_set_text(tooltip, t.second.text.c_str());
      return true;
    }
  }
  return false;
}

}  // namespace nu
