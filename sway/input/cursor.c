#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <errno.h>
#include <time.h>
#include <strings.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/types/wlr_tablet_pad.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/region.h>
#include "config.h"
#include "log.h"
#include "util.h"
#include "sway/commands.h"
#include "sway/input/cursor.h"
#include "sway/input/keyboard.h"
#include "sway/input/tablet.h"
#include "sway/layers.h"
#include "sway/output.h"
#include "sway/scene_descriptor.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

static uint32_t get_current_time_msec(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void sway_cursor_move(struct sway_cursor *cursor, double delta_x, double delta_y) {
	sway_cursor_warp(cursor, cursor->x + delta_x, cursor->y + delta_y);
}

void sway_cursor_warp(struct sway_cursor *cursor, double x, double y) {
	wlr_output_layout_closest_point(root->output_layout, NULL,
		x, y, &x, &y);
	cursor->x = x;
	cursor->y = y;
	wlr_scene_node_set_position(&cursor->scene->node,
		(int)round(cursor->x),
		(int)round(cursor->y));
}

/**
 * Returns the node at the cursor's position. If there is a surface at that
 * location, it is stored in **surface (it may not be a view).
 */
struct sway_node *node_at_coords(
		struct sway_seat *seat, double lx, double ly,
		struct wlr_surface **_surface, double *sx, double *sy) {
	struct wlr_scene_node *scene_node = NULL;
	size_t num_layers = sizeof(root->layers) / sizeof(struct wlr_scene_tree *);
	for (int i = num_layers - 1; i >= 0 && !scene_node; i--) {
		struct wlr_scene_tree *layer = ((struct wlr_scene_tree **) &root->layers)[i];

		bool non_interactive = scene_descriptor_try_get(&layer->node,
			SWAY_SCENE_DESC_NON_INTERACTIVE);

		if (!non_interactive) {
			scene_node = wlr_scene_node_at(&layer->node, lx, ly, sx, sy);
		}
	}

	if (scene_node) {
		// determine what wlr_surface we clicked on
		if (scene_node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *scene_buffer =
				wlr_scene_buffer_from_node(scene_node);
			struct wlr_scene_surface *scene_surface =
				wlr_scene_surface_try_from_buffer(scene_buffer);

			if (scene_surface) {
				*_surface = scene_surface->surface;
			}
		}

		// determine what container we clicked on
		struct wlr_scene_node *current = scene_node;
		while (true) {
			struct sway_container *con = scene_descriptor_try_get(current,
				SWAY_SCENE_DESC_CONTAINER);

			if (!con) {
				struct sway_view *view = scene_descriptor_try_get(current,
					SWAY_SCENE_DESC_VIEW);
				if (view) {
					con = view->container;
				}
			}

			if (!con) {
				struct sway_xdg_popup *popup =
					scene_descriptor_try_get(current, SWAY_SCENE_DESC_POPUP);
				if (popup) {
					con = popup->view->container;
				}
			}

			if (con && (!con->view || con->view->surface)) {
				return &con->node;
			}

			if (scene_descriptor_try_get(current, SWAY_SCENE_DESC_LAYER_SHELL)) {
				// We don't want to feed through the current workspace on
				// layer shells
				return NULL;
			}

#if HAVE_XWAYLAND
			if (scene_descriptor_try_get(current, SWAY_SCENE_DESC_XWAYLAND_UNMANAGED)) {
				return NULL;
			}
#endif

			if (!current->parent) {
				break;
			}

			current = &current->parent->node;
		}
	}

	// if we aren't on a container, determine what workspace we are on
	struct wlr_output *wlr_output = wlr_output_layout_output_at(
			root->output_layout, lx, ly);
	if (wlr_output == NULL) {
		return NULL;
	}

	struct sway_output *output = wlr_output->data;
	if (!output || !output->enabled) {
		// output is being destroyed or is being enabled
		return NULL;
	}

	struct sway_workspace *ws = output_get_active_workspace(output);
	if (!ws) {
		return NULL;
	}

	return &ws->node;
}

void cursor_rebase(struct sway_cursor *cursor) {
	uint32_t time_msec = get_current_time_msec();
	seatop_rebase(cursor->seat, time_msec);
}

void cursor_rebase_all(void) {
	if (!root->outputs->length) {
		return;
	}

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		cursor_rebase(seat->cursor);
	}
}

void cursor_update_image(struct sway_cursor *cursor,
		struct sway_node *node) {
	if (node && node->type == N_CONTAINER) {
		// Try a node's resize edge
		enum wlr_edges edge = find_resize_edge(node->sway_container, NULL, cursor);
		if (edge == WLR_EDGE_NONE) {
			cursor_set_image(cursor, "default", NULL);
		} else if (container_is_floating(node->sway_container)) {
			cursor_set_image(cursor, wlr_xcursor_get_resize_name(edge), NULL);
		} else {
			if (edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
				cursor_set_image(cursor, "col-resize", NULL);
			} else {
				cursor_set_image(cursor, "row-resize", NULL);
			}
		}
	} else {
		cursor_set_image(cursor, "default", NULL);
	}
}

static void cursor_hide(struct sway_cursor *cursor) {
	wlr_scene_node_set_enabled(&cursor->scene->node, false);
	wlr_seat_pointer_notify_clear_focus(cursor->seat->wlr_seat);
}

static int hide_notify(void *data) {
	struct sway_cursor *cursor = data;
	cursor_hide(cursor);
	return 1;
}

int cursor_get_timeout(struct sway_cursor *cursor) {
	if (cursor->pressed_button_count > 0) {
		// Do not hide cursor unless all buttons are released
		return 0;
	}

	struct seat_config *sc = seat_get_config(cursor->seat);
	if (!sc) {
		sc = seat_get_config_by_name("*");
	}
	int timeout = sc ? sc->hide_cursor_timeout : 0;
	if (timeout < 0) {
		timeout = 0;
	}
	return timeout;
}

void cursor_notify_key_press(struct sway_cursor *cursor) {
	if (!cursor->scene->node.enabled) {
		return;
	}

	if (cursor->hide_when_typing == HIDE_WHEN_TYPING_DEFAULT) {
		// No cached value, need to lookup in the seat_config
		const struct seat_config *seat_config = seat_get_config(cursor->seat);
		if (!seat_config) {
			seat_config = seat_get_config_by_name("*");
			if (!seat_config) {
				return;
			}
		}
		cursor->hide_when_typing = seat_config->hide_cursor_when_typing;
		// The default is currently disabled
		if (cursor->hide_when_typing == HIDE_WHEN_TYPING_DEFAULT) {
			cursor->hide_when_typing = HIDE_WHEN_TYPING_DISABLE;
		}
	}

	if (cursor->hide_when_typing == HIDE_WHEN_TYPING_ENABLE) {
		cursor_hide(cursor);
	}
}

static enum sway_input_idle_source idle_source_from_device(
		struct wlr_input_device *device) {
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		return IDLE_SOURCE_KEYBOARD;
	case WLR_INPUT_DEVICE_POINTER:
		return IDLE_SOURCE_POINTER;
	case WLR_INPUT_DEVICE_TOUCH:
		return IDLE_SOURCE_TOUCH;
	case WLR_INPUT_DEVICE_TABLET_TOOL:
		return IDLE_SOURCE_TABLET_TOOL;
	case WLR_INPUT_DEVICE_TABLET_PAD:
		return IDLE_SOURCE_TABLET_PAD;
	case WLR_INPUT_DEVICE_SWITCH:
		return IDLE_SOURCE_SWITCH;
	}

	abort();
}

void cursor_handle_activity_from_idle_source(struct sway_cursor *cursor,
		enum sway_input_idle_source idle_source) {
	wl_event_source_timer_update(
			cursor->hide_source, cursor_get_timeout(cursor));

	seat_idle_notify_activity(cursor->seat, idle_source);
	if (idle_source != IDLE_SOURCE_TOUCH) {
		cursor_unhide(cursor);
	}
}

void cursor_handle_activity_from_device(struct sway_cursor *cursor,
		struct wlr_input_device *device) {
	enum sway_input_idle_source idle_source = idle_source_from_device(device);
	cursor_handle_activity_from_idle_source(cursor, idle_source);
}

void cursor_unhide(struct sway_cursor *cursor) {
	if (cursor->scene->node.enabled) {
		return;
	}

	wlr_scene_node_set_enabled(&cursor->scene->node, true);

	cursor_rebase(cursor);
	wl_event_source_timer_update(cursor->hide_source, cursor_get_timeout(cursor));
}

void pointer_motion(struct sway_cursor *cursor, uint32_t time_msec,
		struct wlr_input_device *device, double dx, double dy,
		double dx_unaccel, double dy_unaccel) {
	wlr_relative_pointer_manager_v1_send_relative_motion(
		server.relative_pointer_manager,
		cursor->seat->wlr_seat, (uint64_t)time_msec * 1000,
		dx, dy, dx_unaccel, dy_unaccel);

	// Only apply pointer constraints to real pointer input.
	if (cursor->active_constraint && device->type == WLR_INPUT_DEVICE_POINTER) {
		struct wlr_surface *surface = NULL;
		double sx, sy;
		node_at_coords(cursor->seat,
			cursor->x, cursor->y, &surface, &sx, &sy);

		if (cursor->active_constraint->surface != surface) {
			return;
		}

		double sx_confined, sy_confined;
		if (!wlr_region_confine(&cursor->confine, sx, sy, sx + dx, sy + dy,
				&sx_confined, &sy_confined)) {
			return;
		}

		dx = sx_confined - sx;
		dy = sy_confined - sy;
	}

	sway_cursor_move(cursor, dx, dy);
	seatop_pointer_motion(cursor->seat, time_msec);
}

static void handle_pointer_motion_relative(
		struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor = wl_container_of(listener, cursor, motion);
	struct wlr_pointer_motion_event *e = data;
	cursor_handle_activity_from_device(cursor->cursor, &e->pointer->base);

	pointer_motion(cursor->cursor, e->time_msec, &e->pointer->base, e->delta_x,
		e->delta_y, e->unaccel_dx, e->unaccel_dy);
}

static void handle_pointer_motion_absolute(
		struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor =
		wl_container_of(listener, cursor, motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->pointer->base);

	struct wlr_box mapping;
	wlr_output_layout_get_box(root->output_layout, NULL, &mapping);

	double dx = (event->x * mapping.width + mapping.x) - cursor->cursor->x;
	double dy = (event->y * mapping.height + mapping.y) - cursor->cursor->y;

	pointer_motion(cursor->cursor, event->time_msec, &event->pointer->base, dx, dy,
		dx, dy);
}

void dispatch_cursor_button(struct sway_cursor *cursor,
		struct wlr_input_device *device, uint32_t time_msec, uint32_t button,
		enum wlr_button_state state) {
	if (time_msec == 0) {
		time_msec = get_current_time_msec();
	}

	seatop_button(cursor->seat, time_msec, device, button, state);
}

static void handle_pointer_button(struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor = wl_container_of(listener, cursor, button);
	struct wlr_pointer_button_event *event = data;

	if (event->state == WLR_BUTTON_PRESSED) {
		cursor->cursor->pressed_button_count++;
	} else {
		if (cursor->cursor->pressed_button_count > 0) {
			cursor->cursor->pressed_button_count--;
		} else {
			sway_log(SWAY_ERROR, "Pressed button count was wrong");
		}
	}

	cursor_handle_activity_from_device(cursor->cursor, &event->pointer->base);
	dispatch_cursor_button(cursor->cursor, &event->pointer->base,
			event->time_msec, event->button, event->state);
}

void dispatch_cursor_axis(struct sway_cursor *cursor,
		struct wlr_pointer_axis_event *event) {
	seatop_pointer_axis(cursor->seat, event);
}

static void handle_pointer_axis(struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor = wl_container_of(listener, cursor, axis);
	struct wlr_pointer_axis_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->pointer->base);
	dispatch_cursor_axis(cursor->cursor, event);
}

static void handle_pointer_frame(struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor = wl_container_of(listener, cursor, frame);
	wlr_seat_pointer_notify_frame(cursor->cursor->seat->wlr_seat);
}

static void touch_to_layout_coords(double *x, double *y) {
	struct wlr_box mapping;
	wlr_output_layout_get_box(root->output_layout, NULL, &mapping);

	*x = *x * mapping.width + mapping.x;
	*y = *y * mapping.height + mapping.y;
}

static void handle_touch_down(struct wl_listener *listener, void *data) {
	struct sway_cursor_touch *cursor = wl_container_of(listener, cursor, down);
	struct wlr_touch_down_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->touch->base);
	cursor_hide(cursor->cursor);

	double lx = event->x, ly = event->y;
	touch_to_layout_coords(&lx, &ly);

	struct sway_seat *seat = cursor->cursor->seat;
	seat->touch_id = event->touch_id;
	seat->touch_x = lx;
	seat->touch_y = ly;

	seatop_touch_down(seat, event, lx, ly);
}

static void handle_touch_up(struct wl_listener *listener, void *data) {
	struct sway_cursor_touch *cursor = wl_container_of(listener, cursor, up);
	struct wlr_touch_up_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->touch->base);

	struct sway_seat *seat = cursor->cursor->seat;

	if (cursor->cursor->simulating_pointer_from_touch) {
		if (cursor->cursor->pointer_touch_id == cursor->cursor->seat->touch_id) {
			cursor->cursor->pointer_touch_up = true;
			dispatch_cursor_button(cursor->cursor, &event->touch->base,
				event->time_msec, BTN_LEFT, WLR_BUTTON_RELEASED);
		}
	} else {
		seatop_touch_up(seat, event);
	}
}

static void handle_touch_cancel(struct wl_listener *listener, void *data) {
	struct sway_cursor_touch *cursor =
		wl_container_of(listener, cursor, cancel);
	struct wlr_touch_cancel_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->touch->base);

	struct sway_seat *seat = cursor->cursor->seat;

	if (cursor->cursor->simulating_pointer_from_touch) {
		if (cursor->cursor->pointer_touch_id == cursor->cursor->seat->touch_id) {
			cursor->cursor->pointer_touch_up = true;
			dispatch_cursor_button(cursor->cursor, &event->touch->base,
				event->time_msec, BTN_LEFT, WLR_BUTTON_RELEASED);
		}
	} else {
		seatop_touch_cancel(seat, event);
	}
}

static void handle_touch_motion(struct wl_listener *listener, void *data) {
	struct sway_cursor_touch *cursor =
		wl_container_of(listener, cursor, motion);
	struct wlr_touch_motion_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->touch->base);

	struct sway_seat *seat = cursor->cursor->seat;

	double lx = event->x, ly = event->y;
	touch_to_layout_coords(&lx, &ly);

	if (seat->touch_id == event->touch_id) {
		seat->touch_x = lx;
		seat->touch_y = ly;

		drag_icons_update_position(seat);
	}

	if (cursor->cursor->simulating_pointer_from_touch) {
		if (seat->touch_id == cursor->cursor->pointer_touch_id) {
			double dx, dy;
			dx = lx - cursor->cursor->x;
			dy = ly - cursor->cursor->y;
			pointer_motion(cursor->cursor, event->time_msec, &event->touch->base,
				dx, dy, dx, dy);
		}
	} else {
		seatop_touch_motion(seat, event, lx, ly);
	}
}

static void handle_touch_frame(struct wl_listener *listener, void *data) {
	struct sway_cursor_touch *cursor =
		wl_container_of(listener, cursor, frame);

	struct wlr_seat *wlr_seat = cursor->cursor->seat->wlr_seat;

	if (cursor->cursor->simulating_pointer_from_touch) {
		wlr_seat_pointer_notify_frame(wlr_seat);

		if (cursor->cursor->pointer_touch_up) {
			cursor->cursor->pointer_touch_up = false;
			cursor->cursor->simulating_pointer_from_touch = false;
		}
	} else {
		wlr_seat_touch_notify_frame(wlr_seat);
	}
}

static double apply_mapping_from_coord(double low, double high, double value) {
	if (isnan(value)) {
		return value;
	}

	return (value - low) / (high - low);
}

static void apply_mapping_from_region(struct wlr_input_device *device,
		struct input_config_mapped_from_region *region, double *x, double *y) {
	double x1 = region->x1, x2 = region->x2;
	double y1 = region->y1, y2 = region->y2;

	if (region->mm && device->type == WLR_INPUT_DEVICE_TABLET_TOOL) {
		struct wlr_tablet *tablet = wlr_tablet_from_input_device(device);
		if (tablet->width_mm == 0 || tablet->height_mm == 0) {
			return;
		}
		x1 /= tablet->width_mm;
		x2 /= tablet->width_mm;
		y1 /= tablet->height_mm;
		y2 /= tablet->height_mm;
	}

	*x = apply_mapping_from_coord(x1, x2, *x);
	*y = apply_mapping_from_coord(y1, y2, *y);
}

static void handle_tablet_tool_position(struct sway_cursor *cursor,
		struct sway_tablet_tool *tool,
		bool change_x, bool change_y,
		double x, double y, double dx, double dy,
		int32_t time_msec) {

	if (!change_x && !change_y) {
		return;
	}

	struct sway_tablet *tablet = tool->tablet;
	struct sway_input_device *input_device = tablet->seat_device->input_device;
	struct input_config *ic = input_device_get_config(input_device);
	if (ic != NULL && ic->mapped_from_region != NULL) {
		apply_mapping_from_region(input_device->wlr_device,
			ic->mapped_from_region, &x, &y);
	}

	switch (tool->mode) {
	case SWAY_TABLET_TOOL_MODE_ABSOLUTE: {
		double lx = x, ly = y;
		touch_to_layout_coords(&lx, &ly);
		sway_cursor_warp(cursor, change_x ? lx : cursor->x, change_y ? ly : cursor->y);
		break;
	}
	case SWAY_TABLET_TOOL_MODE_RELATIVE:
		sway_cursor_move(cursor, dx, dy);
		break;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct sway_seat *seat = cursor->seat;
	node_at_coords(seat, cursor->x, cursor->y, &surface, &sx, &sy);

	// The logic for whether we should send a tablet event or an emulated pointer
	// event is tricky. It comes down to:
	// * If we began a drag on a non-tablet surface (simulating_pointer_from_tool_tip),
	//   then we should continue sending emulated pointer events regardless of
	//   whether the surface currently under us accepts tablet or not.
	// * Otherwise, if we are over a surface that accepts tablet, then we should
	//   send tablet events.
	// * If we began a drag over a tablet surface, we should continue sending
	//   tablet events until the drag is released, even if we are now over a
	//   non-tablet surface.
	if (!cursor->simulating_pointer_from_tool_tip &&
			((surface && wlr_surface_accepts_tablet_v2(tablet->tablet_v2, surface)) ||
				wlr_tablet_tool_v2_has_implicit_grab(tool->tablet_v2_tool))) {
		seatop_tablet_tool_motion(seat, tool, time_msec);
	} else {
		wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->tablet_v2_tool);
		pointer_motion(cursor, time_msec, input_device->wlr_device, dx, dy, dx, dy);
	}
}

static void handle_tool_axis(struct wl_listener *listener, void *data) {
	struct sway_cursor_tablet *cursor = wl_container_of(listener, cursor, tool_axis);
	struct wlr_tablet_tool_axis_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->tablet->base);

	struct sway_tablet_tool *sway_tool = event->tool->data;
	if (!sway_tool) {
		sway_log(SWAY_DEBUG, "tool axis before proximity");
		return;
	}

	handle_tablet_tool_position(cursor->cursor, sway_tool,
		event->updated_axes & WLR_TABLET_TOOL_AXIS_X,
		event->updated_axes & WLR_TABLET_TOOL_AXIS_Y,
		event->x, event->y, event->dx, event->dy, event->time_msec);

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE) {
		wlr_tablet_v2_tablet_tool_notify_pressure(
			sway_tool->tablet_v2_tool, event->pressure);
	}

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE) {
		wlr_tablet_v2_tablet_tool_notify_distance(
			sway_tool->tablet_v2_tool, event->distance);
	}

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_X) {
		sway_tool->tilt_x = event->tilt_x;
	}

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_Y) {
		sway_tool->tilt_y = event->tilt_y;
	}

	if (event->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y)) {
		wlr_tablet_v2_tablet_tool_notify_tilt(
			sway_tool->tablet_v2_tool,
			sway_tool->tilt_x, sway_tool->tilt_y);
	}

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION) {
		wlr_tablet_v2_tablet_tool_notify_rotation(
			sway_tool->tablet_v2_tool, event->rotation);
	}

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER) {
		wlr_tablet_v2_tablet_tool_notify_slider(
			sway_tool->tablet_v2_tool, event->slider);
	}

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL) {
		wlr_tablet_v2_tablet_tool_notify_wheel(
			sway_tool->tablet_v2_tool, event->wheel_delta, 0);
	}
}

static void handle_tool_tip(struct wl_listener *listener, void *data) {
	struct sway_cursor_tablet *cursor = wl_container_of(listener, cursor, tool_tip);
	struct wlr_tablet_tool_tip_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->tablet->base);

	struct sway_tablet_tool *sway_tool = event->tool->data;
	struct wlr_tablet_v2_tablet *tablet_v2 = sway_tool->tablet->tablet_v2;
	struct sway_seat *seat = cursor->cursor->seat;


	double sx, sy;
	struct wlr_surface *surface = NULL;
	node_at_coords(seat, cursor->cursor->x, cursor->cursor->y,
		&surface, &sx, &sy);

	if (cursor->cursor->simulating_pointer_from_tool_tip &&
			event->state == WLR_TABLET_TOOL_TIP_UP) {
		cursor->cursor->simulating_pointer_from_tool_tip = false;
		dispatch_cursor_button(cursor->cursor, &event->tablet->base, event->time_msec,
			BTN_LEFT, WLR_BUTTON_RELEASED);
		wlr_seat_pointer_notify_frame(cursor->cursor->seat->wlr_seat);
	} else if (!surface || !wlr_surface_accepts_tablet_v2(tablet_v2, surface)) {
		// If we started holding the tool tip down on a surface that accepts
		// tablet v2, we should notify that surface if it gets released over a
		// surface that doesn't support v2.
		if (event->state == WLR_TABLET_TOOL_TIP_UP) {
			seatop_tablet_tool_tip(seat, sway_tool, event->time_msec,
				WLR_TABLET_TOOL_TIP_UP);
		} else {
			cursor->cursor->simulating_pointer_from_tool_tip = true;
			dispatch_cursor_button(cursor->cursor, &event->tablet->base,
				event->time_msec, BTN_LEFT, WLR_BUTTON_PRESSED);
			wlr_seat_pointer_notify_frame(cursor->cursor->seat->wlr_seat);
		}
	} else {
		seatop_tablet_tool_tip(seat, sway_tool, event->time_msec, event->state);
	}
}

static struct sway_tablet *get_tablet_for_device(struct sway_cursor *cursor,
		struct wlr_input_device *device) {
	struct sway_tablet *tablet;
	wl_list_for_each(tablet, &cursor->tablets, link) {
		if (tablet->seat_device->input_device->wlr_device == device) {
			return tablet;
		}
	}
	return NULL;
}

static void handle_tool_proximity(struct wl_listener *listener, void *data) {
	struct sway_cursor_tablet *cursor =
		wl_container_of(listener, cursor, tool_proximity);
	struct wlr_tablet_tool_proximity_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->tablet->base);

	struct wlr_tablet_tool *tool = event->tool;
	if (!tool->data) {
		struct sway_tablet *tablet = get_tablet_for_device(cursor->cursor,
			&event->tablet->base);
		if (!tablet) {
			sway_log(SWAY_ERROR, "no tablet for tablet tool");
			return;
		}
		sway_tablet_tool_configure(tablet, tool);
	}

	struct sway_tablet_tool *sway_tool = tool->data;
	if (!sway_tool) {
		sway_log(SWAY_ERROR, "tablet tool not initialized");
		return;
	}

	if (event->state == WLR_TABLET_TOOL_PROXIMITY_OUT) {
		wlr_tablet_v2_tablet_tool_notify_proximity_out(sway_tool->tablet_v2_tool);
		return;
	}

	handle_tablet_tool_position(cursor->cursor, sway_tool, true, true, event->x, event->y,
		0, 0, event->time_msec);
}

static void handle_tool_button(struct wl_listener *listener, void *data) {
	struct sway_cursor_tablet *cursor = wl_container_of(listener, cursor, tool_button);
	struct wlr_tablet_tool_button_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->tablet->base);

	struct sway_tablet_tool *sway_tool = event->tool->data;
	if (!sway_tool) {
		sway_log(SWAY_DEBUG, "tool button before proximity");
		return;
	}
	struct wlr_tablet_v2_tablet *tablet_v2 = sway_tool->tablet->tablet_v2;

	double sx, sy;
	struct wlr_surface *surface = NULL;

	node_at_coords(cursor->cursor->seat, cursor->cursor->x, cursor->cursor->y,
		&surface, &sx, &sy);

	// TODO: floating resize should support graphics tablet events
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(cursor->cursor->seat->wlr_seat);
	uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
	bool mod_pressed = modifiers & config->floating_mod;

	bool surface_supports_tablet_events =
		surface && wlr_surface_accepts_tablet_v2(tablet_v2, surface);

	// Simulate pointer when:
	// 1. The modifier key is pressed, OR
	// 2. The surface under the cursor does not support tablet events.
	bool should_simulate_pointer = mod_pressed || !surface_supports_tablet_events;

	// Similar to tool tip, we need to selectively simulate mouse events, but we
	// want to make sure that it is always consistent. Because all tool buttons
	// currently map to BTN_RIGHT, we need to keep count of how many tool
	// buttons are currently pressed down so we can send consistent events.
	//
	// The logic follows:
	// - If we are already simulating the pointer, we should continue to do so
	//   until at least no tool button is held down.
	// - If we should simulate the pointer and no tool button is currently held
	//   down, begin simulating the pointer.
	// - If neither of the above are true, send the tablet events.
	if ((cursor->cursor->tool_buttons > 0 && cursor->cursor->simulating_pointer_from_tool_button)
		|| (cursor->cursor->tool_buttons == 0 && should_simulate_pointer)) {
		cursor->cursor->simulating_pointer_from_tool_button = true;

		// TODO: the user may want to configure which tool buttons are mapped to
		// which simulated pointer buttons
		switch (event->state) {
		case WLR_BUTTON_PRESSED:
			if (cursor->cursor->tool_buttons == 0) {
				dispatch_cursor_button(cursor->cursor, &event->tablet->base,
						event->time_msec, BTN_RIGHT, event->state);
			}
			break;
		case WLR_BUTTON_RELEASED:
			if (cursor->cursor->tool_buttons <= 1) {
				dispatch_cursor_button(cursor->cursor, &event->tablet->base,
						event->time_msec, BTN_RIGHT, event->state);
			}
			break;
		}
		wlr_seat_pointer_notify_frame(cursor->cursor->seat->wlr_seat);
	} else {
		cursor->cursor->simulating_pointer_from_tool_button = false;

		wlr_tablet_v2_tablet_tool_notify_button(sway_tool->tablet_v2_tool,
			event->button, (enum zwp_tablet_pad_v2_button_state)event->state);
	}

	// Update tool button count.
	switch (event->state) {
	case WLR_BUTTON_PRESSED:
		cursor->cursor->tool_buttons++;
		break;
	case WLR_BUTTON_RELEASED:
		if (cursor->cursor->tool_buttons == 0) {
			sway_log(SWAY_ERROR, "inconsistent tablet tool button events");
		} else {
			cursor->cursor->tool_buttons--;
		}
		break;
	}
}

static void check_constraint_region(struct sway_cursor *cursor) {
	struct wlr_pointer_constraint_v1 *constraint = cursor->active_constraint;
	pixman_region32_t *region = &constraint->region;
	struct sway_view *view = view_from_wlr_surface(constraint->surface);
	if (cursor->active_confine_requires_warp && view) {
		cursor->active_confine_requires_warp = false;

		struct sway_container *con = view->container;

		double sx = cursor->x - con->pending.content_x + view->geometry.x;
		double sy = cursor->y - con->pending.content_y + view->geometry.y;

		if (!pixman_region32_contains_point(region,
				floor(sx), floor(sy), NULL)) {
			int nboxes;
			pixman_box32_t *boxes = pixman_region32_rectangles(region, &nboxes);
			if (nboxes > 0) {
				double sx = (boxes[0].x1 + boxes[0].x2) / 2.;
				double sy = (boxes[0].y1 + boxes[0].y2) / 2.;

				sway_cursor_warp(cursor,
					sx + con->pending.content_x - view->geometry.x,
					sy + con->pending.content_y - view->geometry.y);

				cursor_rebase(cursor);
			}
		}
	}

	// A locked pointer will result in an empty region, thus disallowing all movement
	if (constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
		pixman_region32_copy(&cursor->confine, region);
	} else {
		pixman_region32_clear(&cursor->confine);
	}
}

static void handle_constraint_commit(struct wl_listener *listener,
		void *data) {
	struct sway_cursor *cursor =
		wl_container_of(listener, cursor, constraint_commit);
	struct wlr_pointer_constraint_v1 *constraint = cursor->active_constraint;
	assert(constraint->surface == data);

	check_constraint_region(cursor);
}

static void handle_pointer_constraint_set_region(struct wl_listener *listener,
		void *data) {
	struct sway_pointer_constraint *sway_constraint =
		wl_container_of(listener, sway_constraint, set_region);
	struct sway_cursor *cursor = sway_constraint->cursor;

	cursor->active_confine_requires_warp = true;
}

static void handle_request_pointer_set_cursor(struct wl_listener *listener,
		void *data) {
	struct sway_cursor *cursor =
		wl_container_of(listener, cursor, request_set_cursor);
	if (!seatop_allows_set_cursor(cursor->seat)) {
		return;
	}
	struct wlr_seat_pointer_request_set_cursor_event *event = data;

	struct wl_client *focused_client = NULL;
	struct wlr_surface *focused_surface =
		cursor->seat->wlr_seat->pointer_state.focused_surface;
	if (focused_surface != NULL) {
		focused_client = wl_resource_get_client(focused_surface->resource);
	}

	// TODO: check cursor mode
	if (focused_client == NULL ||
			event->seat_client->client != focused_client) {
		sway_log(SWAY_DEBUG, "denying request to set cursor from unfocused client");
		return;
	}

	cursor_set_image_surface(cursor, event->surface, event->hotspot_x,
			event->hotspot_y, focused_client);
}

static void handle_pointer_hold_begin(struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor = wl_container_of(
			listener, cursor, hold_begin);
	struct wlr_pointer_hold_begin_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->pointer->base);
	seatop_hold_begin(cursor->cursor->seat, event);
}

static void handle_pointer_hold_end(struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor = wl_container_of(
			listener, cursor, hold_end);
	struct wlr_pointer_hold_end_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->pointer->base);
	seatop_hold_end(cursor->cursor->seat, event);
}

static void handle_pointer_pinch_begin(struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor = wl_container_of(
			listener, cursor, pinch_begin);
	struct wlr_pointer_pinch_begin_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->pointer->base);
	seatop_pinch_begin(cursor->cursor->seat, event);
}

static void handle_pointer_pinch_update(struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor = wl_container_of(
			listener, cursor, pinch_update);
	struct wlr_pointer_pinch_update_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->pointer->base);
	seatop_pinch_update(cursor->cursor->seat, event);
}

static void handle_pointer_pinch_end(struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor = wl_container_of(
			listener, cursor, pinch_end);
	struct wlr_pointer_pinch_end_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->pointer->base);
	seatop_pinch_end(cursor->cursor->seat, event);
}

static void handle_pointer_swipe_begin(struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor = wl_container_of(
			listener, cursor, swipe_begin);
	struct wlr_pointer_swipe_begin_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->pointer->base);
	seatop_swipe_begin(cursor->cursor->seat, event);
}

static void handle_pointer_swipe_update(struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor = wl_container_of(
			listener, cursor, swipe_update);
	struct wlr_pointer_swipe_update_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->pointer->base);
	seatop_swipe_update(cursor->cursor->seat, event);
}

static void handle_pointer_swipe_end(struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor = wl_container_of(
			listener, cursor, swipe_end);
	struct wlr_pointer_swipe_end_event *event = data;
	cursor_handle_activity_from_device(cursor->cursor, &event->pointer->base);
	seatop_swipe_end(cursor->cursor->seat, event);
}

static void cursor_reset(struct sway_cursor *cursor) {
	struct wlr_scene_tree *tree = cursor->scene;
	struct wlr_scene_node *child, *tmp_child;
	wl_list_for_each_safe(child, tmp_child, &tree->children, link) {
		wlr_scene_node_destroy(child);
	}
}

void cursor_set_image(struct sway_cursor *cursor, const char *image,
		struct wl_client *client) {
	if (!(cursor->seat->wlr_seat->capabilities & WL_SEAT_CAPABILITY_POINTER)) {
		return;
	}

	const char *prev = cursor->image;
	cursor->image = image;

	if (!image) {
		cursor_reset(cursor);
	} else if (!prev || strcmp(prev, image) != 0) {
		cursor_reset(cursor);

		wlr_scene_xcursor_create(cursor->scene, cursor->xcursor_manager, image);
	}
}

void cursor_set_image_surface(struct sway_cursor *cursor,
		struct wlr_surface *surface, int32_t hotspot_x, int32_t hotspot_y,
		struct wl_client *client) {
	if (!(cursor->seat->wlr_seat->capabilities & WL_SEAT_CAPABILITY_POINTER)) {
		return;
	}

	cursor->image = NULL;
	cursor_reset(cursor);

	if (surface) {
		struct wlr_scene_tree *tree = wlr_scene_subsurface_tree_create(cursor->scene, surface);
		if (tree) {
			wlr_scene_node_set_position(&tree->node, -hotspot_x, -hotspot_y);
		}
	}
}

void sway_cursor_destroy(struct sway_cursor *cursor) {
	if (!cursor) {
		return;
	}

	wl_event_source_remove(cursor->hide_source);
	wl_list_remove(&cursor->request_set_cursor.link);

	wlr_xcursor_manager_destroy(cursor->xcursor_manager);
	wlr_scene_node_destroy(&cursor->scene->node);
	free(cursor);
}

struct sway_cursor *sway_cursor_create(struct sway_seat *seat) {
	struct sway_cursor *cursor = calloc(1, sizeof(struct sway_cursor));
	if (!sway_assert(cursor, "could not allocate sway cursor")) {
		return NULL;
	}

	cursor->scene = wlr_scene_tree_create(root->layers.cursor);
	if (!sway_assert(cursor->scene, "could not allocate scene node")) {
		free(cursor);
		return NULL;
	}

	cursor->previous.x = 0;
	cursor->previous.y = 0;

	cursor->seat = seat;
	cursor->hide_source = wl_event_loop_add_timer(server.wl_event_loop,
			hide_notify, cursor);

	wl_signal_add(&seat->wlr_seat->events.request_set_cursor,
			&cursor->request_set_cursor);
	cursor->request_set_cursor.notify = handle_request_pointer_set_cursor;

	wl_list_init(&cursor->constraint_commit.link);
	wl_list_init(&cursor->tablets);
	wl_list_init(&cursor->tablet_pads);

	return cursor;
}

static void handle_cursor_pointer_destroy(struct wl_listener *listener, void *data) {
	struct sway_cursor_pointer *cursor = wl_container_of(listener, cursor, destroy);

	wl_list_remove(&cursor->hold_begin.link);
	wl_list_remove(&cursor->hold_end.link);
	wl_list_remove(&cursor->pinch_begin.link);
	wl_list_remove(&cursor->pinch_update.link);
	wl_list_remove(&cursor->pinch_end.link);
	wl_list_remove(&cursor->swipe_begin.link);
	wl_list_remove(&cursor->swipe_update.link);
	wl_list_remove(&cursor->swipe_end.link);
	wl_list_remove(&cursor->motion.link);
	wl_list_remove(&cursor->motion_absolute.link);
	wl_list_remove(&cursor->button.link);
	wl_list_remove(&cursor->axis.link);
	wl_list_remove(&cursor->destroy.link);

	free(cursor);
}

struct sway_cursor_pointer *sway_cursor_pointer_create(struct sway_cursor *scursor, struct wlr_pointer *pointer) {
	struct sway_cursor_pointer *cursor = calloc(1, sizeof(*cursor));
	if (!cursor) {
		return NULL;
	}

	cursor->cursor = scursor;
	cursor->wlr_pointer = pointer;

	wl_signal_add(&pointer->events.hold_begin, &cursor->hold_begin);
	cursor->hold_begin.notify = handle_pointer_hold_begin;
	wl_signal_add(&pointer->events.hold_end, &cursor->hold_end);
	cursor->hold_end.notify = handle_pointer_hold_end;

	wl_signal_add(&pointer->events.pinch_begin, &cursor->pinch_begin);
	cursor->pinch_begin.notify = handle_pointer_pinch_begin;
	wl_signal_add(&pointer->events.pinch_update, &cursor->pinch_update);
	cursor->pinch_update.notify = handle_pointer_pinch_update;
	wl_signal_add(&pointer->events.pinch_end, &cursor->pinch_end);
	cursor->pinch_end.notify = handle_pointer_pinch_end;

	wl_signal_add(&pointer->events.swipe_begin, &cursor->swipe_begin);
	cursor->swipe_begin.notify = handle_pointer_swipe_begin;
	wl_signal_add(&pointer->events.swipe_update, &cursor->swipe_update);
	cursor->swipe_update.notify = handle_pointer_swipe_update;
	wl_signal_add(&pointer->events.swipe_end, &cursor->swipe_end);
	cursor->swipe_end.notify = handle_pointer_swipe_end;

	// input events
	wl_signal_add(&pointer->events.motion, &cursor->motion);
	cursor->motion.notify = handle_pointer_motion_relative;
	wl_signal_add(&pointer->events.motion_absolute,
		&cursor->motion_absolute);
	cursor->motion_absolute.notify = handle_pointer_motion_absolute;
	wl_signal_add(&pointer->events.button, &cursor->button);
	cursor->button.notify = handle_pointer_button;
	wl_signal_add(&pointer->events.axis, &cursor->axis);
	cursor->axis.notify = handle_pointer_axis;
	wl_signal_add(&pointer->events.frame, &cursor->frame);
	cursor->frame.notify = handle_pointer_frame;

	wl_signal_add(&pointer->base.events.destroy, &cursor->destroy);
	cursor->destroy.notify = handle_cursor_pointer_destroy;

	return cursor;
}

static void handle_cursor_tablet_destroy(struct wl_listener *listener, void *data) {
	struct sway_cursor_tablet *cursor = wl_container_of(listener, cursor, destroy);

	wl_list_remove(&cursor->tool_axis.link);
	wl_list_remove(&cursor->tool_tip.link);
	wl_list_remove(&cursor->tool_button.link);
	wl_list_remove(&cursor->destroy.link);

	free(cursor);
}

struct sway_cursor_tablet *sway_cursor_tablet_create(struct sway_cursor *scursor, struct wlr_tablet *tablet) {
	struct sway_cursor_tablet *cursor = calloc(1, sizeof(*cursor));
	if (!cursor) {
		return NULL;
	}

	cursor->cursor = scursor;
	cursor->wlr_tablet = tablet;

	wl_signal_add(&tablet->events.axis, &cursor->tool_axis);
	cursor->tool_axis.notify = handle_tool_axis;
	wl_signal_add(&tablet->events.tip, &cursor->tool_tip);
	cursor->tool_tip.notify = handle_tool_tip;
	wl_signal_add(&tablet->events.proximity, &cursor->tool_proximity);
	cursor->tool_proximity.notify = handle_tool_proximity;
	wl_signal_add(&tablet->events.button, &cursor->tool_button);
	cursor->tool_button.notify = handle_tool_button;

	wl_signal_add(&tablet->base.events.destroy, &cursor->destroy);
	cursor->destroy.notify = handle_cursor_tablet_destroy;

	return cursor;
}

static void handle_cursor_touch_destroy(struct wl_listener *listener, void *data) {
	struct sway_cursor_touch *cursor = wl_container_of(listener, cursor, destroy);

	wl_list_remove(&cursor->down.link);
	wl_list_remove(&cursor->up.link);
	wl_list_remove(&cursor->motion.link);
	wl_list_remove(&cursor->cancel.link);
	wl_list_remove(&cursor->frame.link);
	wl_list_remove(&cursor->destroy.link);

	free(cursor);
}

struct sway_cursor_touch *sway_cursor_touch_create(struct sway_cursor *scursor, struct wlr_touch *touch) {
	struct sway_cursor_touch *cursor = calloc(1, sizeof(*cursor));
	if (!cursor) {
		return NULL;
	}

	cursor->cursor = scursor;
	cursor->wlr_touch = touch;

	wl_signal_add(&touch->events.down, &cursor->down);
	cursor->down.notify = handle_touch_down;
	wl_signal_add(&touch->events.up, &cursor->up);
	cursor->up.notify = handle_touch_up;
	wl_signal_add(&touch->events.motion, &cursor->motion);
	cursor->motion.notify = handle_touch_motion;
	wl_signal_add(&touch->events.cancel, &cursor->cancel);
	cursor->cancel.notify = handle_touch_cancel;
	wl_signal_add(&touch->events.frame, &cursor->frame);
	cursor->frame.notify = handle_touch_frame;

	wl_signal_add(&touch->base.events.destroy, &cursor->destroy);
	cursor->destroy.notify = handle_cursor_touch_destroy;

	return cursor;
}

/**
 * Warps the cursor to the middle of the container argument.
 * Does nothing if the cursor is already inside the container and `force` is
 * false. If container is NULL, returns without doing anything.
 */
void cursor_warp_to_container(struct sway_cursor *cursor,
		struct sway_container *container, bool force) {
	if (!container) {
		return;
	}

	struct wlr_box box;
	container_get_box(container, &box);
	if (!force && wlr_box_contains_point(&box, cursor->x,
			cursor->y)) {
		return;
	}

	double x = container->pending.x + container->pending.width / 2.0;
	double y = container->pending.y + container->pending.height / 2.0;

	sway_cursor_warp(cursor, x, y);
	cursor_unhide(cursor);
}

/**
 * Warps the cursor to the middle of the workspace argument.
 * If workspace is NULL, returns without doing anything.
 */
void cursor_warp_to_workspace(struct sway_cursor *cursor,
		struct sway_workspace *workspace) {
	if (!workspace) {
		return;
	}

	double x = workspace->x + workspace->width / 2.0;
	double y = workspace->y + workspace->height / 2.0;

	sway_cursor_warp(cursor, x, y);
	cursor_unhide(cursor);
}

uint32_t get_mouse_bindsym(const char *name, char **error) {
	if (strncasecmp(name, "button", strlen("button")) == 0) {
		// Map to x11 mouse buttons
		int number = name[strlen("button")] - '0';
		if (number < 1 || number > 9 || strlen(name) > strlen("button0")) {
			*error = strdup("Only buttons 1-9 are supported. For other mouse "
					"buttons, use the name of the event code.");
			return 0;
		}
		static const uint32_t buttons[] = {BTN_LEFT, BTN_MIDDLE, BTN_RIGHT,
			SWAY_SCROLL_UP, SWAY_SCROLL_DOWN, SWAY_SCROLL_LEFT,
			SWAY_SCROLL_RIGHT, BTN_SIDE, BTN_EXTRA};
		return buttons[number - 1];
	} else if (strncmp(name, "BTN_", strlen("BTN_")) == 0) {
		// Get event code from name
		int code = libevdev_event_code_from_name(EV_KEY, name);
		if (code == -1) {
			*error = format_str("Unknown event %s", name);
			return 0;
		}
		return code;
	}
	return 0;
}

uint32_t get_mouse_bindcode(const char *name, char **error) {
	// Validate event code
	errno = 0;
	char *endptr;
	int code = strtol(name, &endptr, 10);
	if (endptr == name && code <= 0) {
		*error = strdup("Button event code must be a positive integer.");
		return 0;
	} else if (errno == ERANGE) {
		*error = strdup("Button event code out of range.");
		return 0;
	}
	const char *event = libevdev_event_code_get_name(EV_KEY, code);
	if (!event || strncmp(event, "BTN_", strlen("BTN_")) != 0) {
		*error = format_str("Event code %d (%s) is not a button",
			code, event ? event : "(null)");
		return 0;
	}
	return code;
}

uint32_t get_mouse_button(const char *name, char **error) {
	uint32_t button = get_mouse_bindsym(name, error);
	if (!button && !*error) {
		button = get_mouse_bindcode(name, error);
	}
	return button;
}

const char *get_mouse_button_name(uint32_t button) {
	const char *name = libevdev_event_code_get_name(EV_KEY, button);
	if (!name) {
		if (button == SWAY_SCROLL_UP) {
			name = "SWAY_SCROLL_UP";
		} else if (button == SWAY_SCROLL_DOWN) {
			name = "SWAY_SCROLL_DOWN";
		} else if (button == SWAY_SCROLL_LEFT) {
			name = "SWAY_SCROLL_LEFT";
		} else if (button == SWAY_SCROLL_RIGHT) {
			name = "SWAY_SCROLL_RIGHT";
		}
	}
	return name;
}

static void warp_to_constraint_cursor_hint(struct sway_cursor *cursor) {
	struct wlr_pointer_constraint_v1 *constraint = cursor->active_constraint;

	if (constraint->current.committed &
			WLR_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT) {
		double sx = constraint->current.cursor_hint.x;
		double sy = constraint->current.cursor_hint.y;

		struct sway_view *view = view_from_wlr_surface(constraint->surface);
		if (!view) {
			return;
		}

		struct sway_container *con = view->container;

		double lx = sx + con->pending.content_x - view->geometry.x;
		double ly = sy + con->pending.content_y - view->geometry.y;

		sway_cursor_warp(cursor, lx, ly);

		// Warp the pointer as well, so that on the next pointer rebase we don't
		// send an unexpected synthetic motion event to clients.
		wlr_seat_pointer_warp(constraint->seat, sx, sy);
	}
}

void handle_constraint_destroy(struct wl_listener *listener, void *data) {
	struct sway_pointer_constraint *sway_constraint =
		wl_container_of(listener, sway_constraint, destroy);
	struct wlr_pointer_constraint_v1 *constraint = data;
	struct sway_cursor *cursor = sway_constraint->cursor;

	wl_list_remove(&sway_constraint->set_region.link);
	wl_list_remove(&sway_constraint->destroy.link);

	if (cursor->active_constraint == constraint) {
		warp_to_constraint_cursor_hint(cursor);

		if (cursor->constraint_commit.link.next != NULL) {
			wl_list_remove(&cursor->constraint_commit.link);
		}
		wl_list_init(&cursor->constraint_commit.link);
		cursor->active_constraint = NULL;
	}

	free(sway_constraint);
}

void handle_pointer_constraint(struct wl_listener *listener, void *data) {
	struct wlr_pointer_constraint_v1 *constraint = data;
	struct sway_seat *seat = constraint->seat->data;

	struct sway_pointer_constraint *sway_constraint =
		calloc(1, sizeof(struct sway_pointer_constraint));
	sway_constraint->cursor = seat->cursor;
	sway_constraint->constraint = constraint;

	sway_constraint->set_region.notify = handle_pointer_constraint_set_region;
	wl_signal_add(&constraint->events.set_region, &sway_constraint->set_region);

	sway_constraint->destroy.notify = handle_constraint_destroy;
	wl_signal_add(&constraint->events.destroy, &sway_constraint->destroy);

	struct wlr_surface *surface = seat->wlr_seat->keyboard_state.focused_surface;
	if (surface && surface == constraint->surface) {
		sway_cursor_constrain(seat->cursor, constraint);
	}
}

void sway_cursor_constrain(struct sway_cursor *cursor,
		struct wlr_pointer_constraint_v1 *constraint) {
	struct seat_config *config = seat_get_config(cursor->seat);
	if (!config) {
		config = seat_get_config_by_name("*");
	}

	if (!config || config->allow_constrain == CONSTRAIN_DISABLE) {
		return;
	}

	if (cursor->active_constraint == constraint) {
		return;
	}

	wl_list_remove(&cursor->constraint_commit.link);
	if (cursor->active_constraint) {
		if (constraint == NULL) {
			warp_to_constraint_cursor_hint(cursor);
		}
		wlr_pointer_constraint_v1_send_deactivated(
			cursor->active_constraint);
	}

	cursor->active_constraint = constraint;

	if (constraint == NULL) {
		wl_list_init(&cursor->constraint_commit.link);
		return;
	}

	cursor->active_confine_requires_warp = true;

	// FIXME: Big hack, stolen from wlr_pointer_constraints_v1.c:121.
	// This is necessary because the focus may be set before the surface
	// has finished committing, which means that warping won't work properly,
	// since this code will be run *after* the focus has been set.
	// That is why we duplicate the code here.
	if (pixman_region32_not_empty(&constraint->current.region)) {
		pixman_region32_intersect(&constraint->region,
			&constraint->surface->input_region, &constraint->current.region);
	} else {
		pixman_region32_copy(&constraint->region,
			&constraint->surface->input_region);
	}

	check_constraint_region(cursor);

	wlr_pointer_constraint_v1_send_activated(constraint);

	cursor->constraint_commit.notify = handle_constraint_commit;
	wl_signal_add(&constraint->surface->events.commit,
		&cursor->constraint_commit);
}

void handle_request_set_cursor_shape(struct wl_listener *listener, void *data) {
	const struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	struct sway_seat *seat = event->seat_client->seat->data;

	if (!seatop_allows_set_cursor(seat)) {
		return;
	}

	struct wl_client *focused_client = NULL;
	struct wlr_surface *focused_surface = seat->wlr_seat->pointer_state.focused_surface;
	if (focused_surface != NULL) {
		focused_client = wl_resource_get_client(focused_surface->resource);
	}

	// TODO: check cursor mode
	if (focused_client == NULL || event->seat_client->client != focused_client) {
		sway_log(SWAY_DEBUG, "denying request to set cursor from unfocused client");
		return;
	}

	cursor_set_image(seat->cursor, wlr_cursor_shape_v1_name(event->shape), focused_client);
}
