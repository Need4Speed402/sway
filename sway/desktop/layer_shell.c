#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_damage.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_subcompositor.h>
#include "log.h"
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/layers.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tree/workspace.h"

static struct sway_layer_surface *find_mapped_layer_by_client(
		struct wl_client *client, struct wlr_output *ignore_output) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		if (output->wlr_output == ignore_output) {
			continue;
		}
		// For now we'll only check the overlay layer
		/*struct sway_layer_surface *lsurface;
		wl_list_for_each(lsurface,
				&output->layers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], link) {
			struct wl_resource *resource = lsurface->layer_surface->resource;
			if (wl_resource_get_client(resource) == client
					&& lsurface->layer_surface->mapped) {
				return lsurface;
			}
		}*/
	}
	return NULL;
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
	struct sway_layer_surface *sway_layer =
		wl_container_of(listener, sway_layer, output_destroy);
	// Determine if this layer is being used by an exclusive client. If it is,
	// try and find another layer owned by this client to pass focus to.
	struct sway_seat *seat = input_manager_get_default_seat();
	struct wl_client *client =
		wl_resource_get_client(sway_layer->layer_surface->resource);
	bool set_focus = seat->exclusive_client == client;

	wl_list_remove(&sway_layer->output_destroy.link);
	wl_list_remove(&sway_layer->link);
	wl_list_init(&sway_layer->link);

	if (set_focus) {
		struct sway_layer_surface *layer =
			find_mapped_layer_by_client(client, sway_layer->layer_surface->output);
		if (layer) {
			seat_set_focus_layer(seat, layer->layer_surface);
		}
	}

	sway_layer->layer_surface->output = NULL;
	wlr_layer_surface_v1_destroy(sway_layer->layer_surface);
}

static void handle_surface_commit(struct wl_listener *listener, void *data) {
}

static void unmap(struct sway_layer_surface *sway_layer) {
	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		if (seat->focused_layer == sway_layer->layer_surface) {
			seat_set_focus_layer(seat, NULL);
		}
	}

	cursor_rebase_all();

	struct wlr_output *wlr_output = sway_layer->layer_surface->output;
	if (wlr_output == NULL) {
		return;
	}
	struct sway_output *output = wlr_output->data;
	if (output == NULL) {
		return;
	}
}

static void layer_subsurface_destroy(struct sway_layer_subsurface *subsurface);

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct sway_layer_surface *sway_layer =
		wl_container_of(listener, sway_layer, destroy);
	sway_log(SWAY_DEBUG, "Layer surface destroyed (%s)",
		sway_layer->layer_surface->namespace);
	if (sway_layer->layer_surface->mapped) {
		unmap(sway_layer);
	}

	struct sway_layer_subsurface *subsurface, *subsurface_tmp;
	wl_list_for_each_safe(subsurface, subsurface_tmp, &sway_layer->subsurfaces, link) {
		layer_subsurface_destroy(subsurface);
	}

	wl_list_remove(&sway_layer->link);
	wl_list_remove(&sway_layer->destroy.link);
	wl_list_remove(&sway_layer->map.link);
	wl_list_remove(&sway_layer->unmap.link);
	wl_list_remove(&sway_layer->surface_commit.link);
	wl_list_remove(&sway_layer->new_popup.link);
	wl_list_remove(&sway_layer->new_subsurface.link);
	if (sway_layer->layer_surface->output != NULL) {
		struct sway_output *output = sway_layer->layer_surface->output->data;
		if (output != NULL) {
			transaction_commit_dirty();
		}
		wl_list_remove(&sway_layer->output_destroy.link);
		sway_layer->layer_surface->output = NULL;
	}
	free(sway_layer);
}

static void handle_map(struct wl_listener *listener, void *data) {
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct sway_layer_surface *sway_layer = wl_container_of(
			listener, sway_layer, unmap);
	unmap(sway_layer);
}

static void subsurface_damage(struct sway_layer_subsurface *subsurface,
		bool whole) {
}

static void subsurface_handle_unmap(struct wl_listener *listener, void *data) {
	struct sway_layer_subsurface *subsurface =
			wl_container_of(listener, subsurface, unmap);
	subsurface_damage(subsurface, true);
}

static void subsurface_handle_map(struct wl_listener *listener, void *data) {
	struct sway_layer_subsurface *subsurface =
			wl_container_of(listener, subsurface, map);
	subsurface_damage(subsurface, true);
}

static void subsurface_handle_commit(struct wl_listener *listener, void *data) {
	struct sway_layer_subsurface *subsurface =
			wl_container_of(listener, subsurface, commit);
	subsurface_damage(subsurface, false);
}

static void layer_subsurface_destroy(struct sway_layer_subsurface *subsurface) {
	wl_list_remove(&subsurface->link);
	wl_list_remove(&subsurface->map.link);
	wl_list_remove(&subsurface->unmap.link);
	wl_list_remove(&subsurface->destroy.link);
	wl_list_remove(&subsurface->commit.link);
	free(subsurface);
}

static void subsurface_handle_destroy(struct wl_listener *listener,
		void *data) {
	struct sway_layer_subsurface *subsurface =
			wl_container_of(listener, subsurface, destroy);
	layer_subsurface_destroy(subsurface);
}

static struct sway_layer_subsurface *create_subsurface(
		struct wlr_subsurface *wlr_subsurface,
		struct sway_layer_surface *layer_surface) {
	struct sway_layer_subsurface *subsurface =
			calloc(1, sizeof(struct sway_layer_subsurface));
	if (subsurface == NULL) {
		return NULL;
	}

	subsurface->wlr_subsurface = wlr_subsurface;
	subsurface->layer_surface = layer_surface;
	wl_list_insert(&layer_surface->subsurfaces, &subsurface->link);

	subsurface->map.notify = subsurface_handle_map;
	wl_signal_add(&wlr_subsurface->events.map, &subsurface->map);
	subsurface->unmap.notify = subsurface_handle_unmap;
	wl_signal_add(&wlr_subsurface->events.unmap, &subsurface->unmap);
	subsurface->destroy.notify = subsurface_handle_destroy;
	wl_signal_add(&wlr_subsurface->events.destroy, &subsurface->destroy);
	subsurface->commit.notify = subsurface_handle_commit;
	wl_signal_add(&wlr_subsurface->surface->events.commit, &subsurface->commit);

	return subsurface;
}

static void handle_new_subsurface(struct wl_listener *listener, void *data) {
	struct sway_layer_surface *sway_layer_surface =
			wl_container_of(listener, sway_layer_surface, new_subsurface);
	struct wlr_subsurface *wlr_subsurface = data;
	create_subsurface(wlr_subsurface, sway_layer_surface);
}


static struct sway_layer_surface *popup_get_layer(
		struct sway_layer_popup *popup) {
	while (popup->parent_type == LAYER_PARENT_POPUP) {
		popup = popup->parent_popup;
	}
	return popup->parent_layer;
}

static void popup_damage(struct sway_layer_popup *layer_popup, bool whole) {
	struct wlr_xdg_popup *popup = layer_popup->wlr_popup;
	int popup_sx = popup->geometry.x - popup->base->current.geometry.x;
	int popup_sy = popup->geometry.y - popup->base->current.geometry.y;
	int ox = popup_sx, oy = popup_sy;
	struct sway_layer_surface *layer;
	while (true) {
		if (layer_popup->parent_type == LAYER_PARENT_POPUP) {
			layer_popup = layer_popup->parent_popup;
			ox += layer_popup->wlr_popup->geometry.x;
			oy += layer_popup->wlr_popup->geometry.y;
		} else {
			layer = layer_popup->parent_layer;
			ox += layer->geo.x;
			oy += layer->geo.y;
			break;
		}
	}
}

static void popup_handle_map(struct wl_listener *listener, void *data) {
	struct sway_layer_popup *popup = wl_container_of(listener, popup, map);
	struct sway_layer_surface *layer = popup_get_layer(popup);
	struct wlr_output *wlr_output = layer->layer_surface->output;
	wlr_surface_send_enter(popup->wlr_popup->base->surface, wlr_output);
	popup_damage(popup, true);
}

static void popup_handle_unmap(struct wl_listener *listener, void *data) {
	struct sway_layer_popup *popup = wl_container_of(listener, popup, unmap);
	popup_damage(popup, true);
}

static void popup_handle_commit(struct wl_listener *listener, void *data) {
	struct sway_layer_popup *popup = wl_container_of(listener, popup, commit);
	popup_damage(popup, false);
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
	struct sway_layer_popup *popup =
		wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->map.link);
	wl_list_remove(&popup->unmap.link);
	wl_list_remove(&popup->destroy.link);
	wl_list_remove(&popup->commit.link);
	free(popup);
}

static void popup_unconstrain(struct sway_layer_popup *popup) {
	struct sway_layer_surface *layer = popup_get_layer(popup);
	struct wlr_xdg_popup *wlr_popup = popup->wlr_popup;

	struct sway_output *output = layer->layer_surface->output->data;

	// the output box expressed in the coordinate system of the toplevel parent
	// of the popup
	struct wlr_box output_toplevel_sx_box = {
		.x = -layer->geo.x,
		.y = -layer->geo.y,
		.width = output->width,
		.height = output->height,
	};

	wlr_xdg_popup_unconstrain_from_box(wlr_popup, &output_toplevel_sx_box);
}

static void popup_handle_new_popup(struct wl_listener *listener, void *data);

static struct sway_layer_popup *create_popup(struct wlr_xdg_popup *wlr_popup,
		enum layer_parent parent_type, void *parent) {
	struct sway_layer_popup *popup =
		calloc(1, sizeof(struct sway_layer_popup));
	if (popup == NULL) {
		return NULL;
	}

	popup->wlr_popup = wlr_popup;
	popup->parent_type = parent_type;
	popup->parent_layer = parent;

	popup->map.notify = popup_handle_map;
	wl_signal_add(&wlr_popup->base->events.map, &popup->map);
	popup->unmap.notify = popup_handle_unmap;
	wl_signal_add(&wlr_popup->base->events.unmap, &popup->unmap);
	popup->destroy.notify = popup_handle_destroy;
	wl_signal_add(&wlr_popup->base->events.destroy, &popup->destroy);
	popup->commit.notify = popup_handle_commit;
	wl_signal_add(&wlr_popup->base->surface->events.commit, &popup->commit);
	popup->new_popup.notify = popup_handle_new_popup;
	wl_signal_add(&wlr_popup->base->events.new_popup, &popup->new_popup);

	popup_unconstrain(popup);

	return popup;
}

static void popup_handle_new_popup(struct wl_listener *listener, void *data) {
	struct sway_layer_popup *sway_layer_popup =
		wl_container_of(listener, sway_layer_popup, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	create_popup(wlr_popup, LAYER_PARENT_POPUP, sway_layer_popup);
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
	struct sway_layer_surface *sway_layer_surface =
		wl_container_of(listener, sway_layer_surface, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	create_popup(wlr_popup, LAYER_PARENT_LAYER, sway_layer_surface);
}

struct sway_layer_surface *layer_from_wlr_layer_surface_v1(
		struct wlr_layer_surface_v1 *layer_surface) {
	return layer_surface->data;
}

void handle_layer_shell_surface(struct wl_listener *listener, void *data) {
	struct wlr_layer_surface_v1 *layer_surface = data;
	sway_log(SWAY_DEBUG, "new layer surface: namespace %s layer %d anchor %" PRIu32
			" size %" PRIu32 "x%" PRIu32 " margin %" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",",
		layer_surface->namespace,
		layer_surface->pending.layer,
		layer_surface->pending.anchor,
		layer_surface->pending.desired_width,
		layer_surface->pending.desired_height,
		layer_surface->pending.margin.top,
		layer_surface->pending.margin.right,
		layer_surface->pending.margin.bottom,
		layer_surface->pending.margin.left);

	if (!layer_surface->output) {
		// Assign last active output
		struct sway_output *output = NULL;
		struct sway_seat *seat = input_manager_get_default_seat();
		if (seat) {
			struct sway_workspace *ws = seat_get_focused_workspace(seat);
			if (ws != NULL) {
				output = ws->output;
			}
		}
		if (!output || output == root->fallback_output) {
			if (!root->outputs->length) {
				sway_log(SWAY_ERROR,
						"no output to auto-assign layer surface '%s' to",
						layer_surface->namespace);
				wlr_layer_surface_v1_destroy(layer_surface);
				return;
			}
			output = root->outputs->items[0];
		}
		layer_surface->output = output->wlr_output;
	}

	struct sway_layer_surface *sway_layer =
		calloc(1, sizeof(struct sway_layer_surface));
	if (!sway_layer) {
		return;
	}

	wl_list_init(&sway_layer->subsurfaces);

	sway_layer->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit,
		&sway_layer->surface_commit);

	sway_layer->destroy.notify = handle_destroy;
	wl_signal_add(&layer_surface->events.destroy, &sway_layer->destroy);
	sway_layer->map.notify = handle_map;
	wl_signal_add(&layer_surface->events.map, &sway_layer->map);
	sway_layer->unmap.notify = handle_unmap;
	wl_signal_add(&layer_surface->events.unmap, &sway_layer->unmap);
	sway_layer->new_popup.notify = handle_new_popup;
	wl_signal_add(&layer_surface->events.new_popup, &sway_layer->new_popup);
	sway_layer->new_subsurface.notify = handle_new_subsurface;
	wl_signal_add(&layer_surface->surface->events.new_subsurface,
			&sway_layer->new_subsurface);

	sway_layer->layer_surface = layer_surface;
	layer_surface->data = sway_layer;

	struct sway_output *output = layer_surface->output->data;
	sway_layer->output_destroy.notify = handle_output_destroy;
	wl_signal_add(&output->events.disable, &sway_layer->output_destroy);

	/*
	wl_list_insert(&output->layers[layer_surface->pending.layer],
			&sway_layer->link);
	*/

	// Temporarily set the layer's current state to pending
	// So that we can easily arrange it
	struct wlr_layer_surface_v1_state old_state = layer_surface->current;
	layer_surface->current = layer_surface->pending;
	layer_surface->current = old_state;

	transaction_commit_dirty();
}
