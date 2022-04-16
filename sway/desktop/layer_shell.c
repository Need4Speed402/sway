#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_damage.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_subcompositor.h>
#include "log.h"
#include "sway/scene_descriptor.h"
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/layers.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tree/arrange.h"
#include "sway/tree/workspace.h"
#include <wlr/types/wlr_scene.h>

static void arrange_surface(struct sway_output *output, const struct wlr_box *full_area,
		struct wlr_box *usable_area, struct wlr_scene_node *scene_node) {
	struct wlr_scene_node *node;
	wl_list_for_each(node, &scene_node->state.children, state.link) {
		struct sway_scene_descriptor *desc = node->data;

		sway_assert(desc && desc->type == SWAY_SCENE_DESC_LAYER_SHELL,
			"Corrupted scene tree: expected a layer shell node");

		struct sway_layer_surface *surface = desc->data;
		wlr_scene_layer_surface_v1_configure(surface->scene,
			full_area, usable_area);
	}
}

void arrange_layers(struct sway_output *output) {
	struct wlr_box usable_area = { 0 };
	wlr_output_effective_resolution(output->wlr_output,
			&usable_area.width, &usable_area.height);
	const struct wlr_box full_area = usable_area;

	arrange_surface(output, &full_area, &usable_area, output->layers.shell_background);
	arrange_surface(output, &full_area, &usable_area, output->layers.shell_bottom);
	arrange_surface(output, &full_area, &usable_area, output->layers.shell_top);
	arrange_surface(output, &full_area, &usable_area, output->layers.shell_overlay);

	if (memcmp(&usable_area, &output->usable_area,
				sizeof(struct wlr_box)) != 0) {
		sway_log(SWAY_DEBUG, "Usable area changed, rearranging output");
		memcpy(&output->usable_area, &usable_area, sizeof(struct wlr_box));
		arrange_output(output);
	}
}

static struct wlr_scene_node *sway_layer_get_scene(struct sway_output *output,
		enum zwlr_layer_shell_v1_layer type) {
	switch (type) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return output->layers.shell_background;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return output->layers.shell_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return output->layers.shell_top;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return output->layers.shell_overlay;
	}

	sway_assert(false, "unreachable");
	return NULL;
}

static struct sway_layer_surface *sway_layer_surface_create(
		struct wlr_scene_layer_surface_v1 *scene) {
	struct sway_layer_surface *surface = calloc(1, sizeof(struct sway_layer_surface));
	if (!surface) {
		return NULL;
	}

	surface->scene = scene;

	return surface;
}

static struct sway_layer_surface *find_mapped_layer_by_client(
		struct wl_client *client, struct sway_output *ignore_output) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		if (output == ignore_output) {
			continue;
		}
		// For now we'll only check the overlay layer
		struct wlr_scene_node *node;
		wl_list_for_each (node, &output->layers.shell_overlay->state.children,
				state.link) {
			struct sway_scene_descriptor *desc = node->data;
			struct sway_layer_surface *surface = desc->data;
			struct wlr_layer_surface_v1 *layer_surface =
				surface->scene->layer_surface;
			struct wl_resource *resource = layer_surface->resource;
			if (wl_resource_get_client(resource) == client
					&& layer_surface->mapped) {
				return surface;
			}
		}
	}
	return NULL;
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
	struct sway_layer_surface *layer =
		wl_container_of(listener, layer, output_destroy);

	// Determine if this layer is being used by an exclusive client. If it is,
	// try and find another layer owned by this client to pass focus to.
	struct sway_seat *seat = input_manager_get_default_seat();
	struct wl_client *client =
		wl_resource_get_client(layer->scene->layer_surface->resource);
	bool set_focus = seat->exclusive_client == client;
	if (set_focus) {
		struct sway_layer_surface *consider_layer =
			find_mapped_layer_by_client(client, layer->output);
		if (consider_layer) {
			seat_set_focus_layer(seat, consider_layer->scene->layer_surface);
		}
	}

	wlr_scene_node_destroy(layer->scene->node);
	layer->output = NULL;
}

static void handle_surface_commit(struct wl_listener *listener, void *data) {
	struct sway_layer_surface *surface =
		wl_container_of(listener, surface, surface_commit);

	if (!surface->output) {
		return;
	}

	struct wlr_layer_surface_v1 *layer_surface = surface->scene->layer_surface;
	uint32_t committed = layer_surface->current.committed;

	if (committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		enum zwlr_layer_shell_v1_layer layer_type = layer_surface->current.layer;
		struct wlr_scene_node *output_layer = sway_layer_get_scene(
			surface->output, layer_type);
		wlr_scene_node_reparent(surface->scene->node, output_layer);
	}

	if (committed || layer_surface->mapped != surface->mapped) {
		surface->mapped = layer_surface->mapped;
		arrange_layers(surface->output);
		transaction_commit_dirty();
	}
}

static void handle_map(struct wl_listener *listener, void *data) {
	struct sway_layer_surface *surface = wl_container_of(listener,
			surface, map);

	struct wlr_layer_surface_v1 *layer_surface =
				surface->scene->layer_surface;

	// focus on new surface
	if (layer_surface->current.keyboard_interactive &&
			(layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY ||
			layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP)) {
		struct sway_seat *seat;
		wl_list_for_each(seat, &server.input->seats, link) {
			// but only if the currently focused layer has a lower precedence
			if (!seat->focused_layer ||
					seat->focused_layer->current.layer >= layer_surface->current.layer) {
				seat_set_focus_layer(seat, layer_surface);
			}
		}
		arrange_layers(surface->output);
	}

	cursor_rebase_all();
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct sway_layer_surface *surface = wl_container_of(
			listener, surface, unmap);
	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		if (seat->focused_layer == surface->scene->layer_surface) {
			seat_set_focus_layer(seat, NULL);
		}
	}

	cursor_rebase_all();
}

static void sway_layer_surface_destroy(struct sway_layer_surface *surface) {
	if (surface == NULL) {
		return;
	}

	wl_list_remove(&surface->map.link);
	wl_list_remove(&surface->unmap.link);
	wl_list_remove(&surface->surface_commit.link);
	wl_list_remove(&surface->destroy.link);
	wl_list_remove(&surface->output_destroy.link);

	free(surface);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct sway_layer_surface *surface =
		wl_container_of(listener, surface, destroy);

	if (surface->output) {
		arrange_layers(surface->output);
		transaction_commit_dirty();
	}

	sway_layer_surface_destroy(surface);
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
	struct sway_layer_popup *popup =
		wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->destroy.link);
	wl_list_remove(&popup->new_popup.link);
	free(popup);
}

static struct sway_layer_surface *popup_get_layer(
		struct sway_layer_popup *popup) {
	struct wlr_scene_node *current = popup->scene;
	while (current) {
		if (current->data) {
			struct sway_scene_descriptor *desc = current->data;
			if (desc->type == SWAY_SCENE_DESC_LAYER_SHELL) {
				return desc->data;
			}
		}

		current = current->parent;
	}

	return NULL;
}

static void popup_unconstrain(struct sway_layer_popup *popup) {
	struct sway_layer_surface *surface = popup_get_layer(popup);
	if (!surface) {
		return;
	}
	
	struct wlr_xdg_popup *wlr_popup = popup->wlr_popup;
	struct sway_output *output = surface->output;

	int lx, ly;
	wlr_scene_node_coords(popup->scene, &lx, &ly);

	// the output box expressed in the coordinate system of the toplevel parent
	// of the popup
	struct wlr_box output_toplevel_sx_box = {
		.x = output->lx - lx,
		.y = output->ly - ly,
		.width = output->width,
		.height = output->height,
	};

	wlr_xdg_popup_unconstrain_from_box(wlr_popup, &output_toplevel_sx_box);
}

static void popup_handle_new_popup(struct wl_listener *listener, void *data);

static struct sway_layer_popup *create_popup(struct wlr_xdg_popup *wlr_popup,
		struct wlr_scene_node *parent) {
	struct sway_layer_popup *popup =
		calloc(1, sizeof(struct sway_layer_popup));
	if (popup == NULL) {
		return NULL;
	}

	popup->wlr_popup = wlr_popup;
	popup->scene = wlr_scene_xdg_surface_create(parent,
		wlr_popup->base);

	if (!popup->scene) {
		free(popup);
		return NULL;
	}

	scene_descriptor_assign(popup->scene, SWAY_SCENE_DESC_LAYER_SHELL_POPUP,
		popup);

	popup->destroy.notify = popup_handle_destroy;
	wl_signal_add(&wlr_popup->base->events.destroy, &popup->destroy);
	popup->new_popup.notify = popup_handle_new_popup;
	wl_signal_add(&wlr_popup->base->events.new_popup, &popup->new_popup);

	popup_unconstrain(popup);

	return popup;
}

static void popup_handle_new_popup(struct wl_listener *listener, void *data) {
	struct sway_layer_popup *sway_layer_popup =
		wl_container_of(listener, sway_layer_popup, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	create_popup(wlr_popup, sway_layer_popup->scene);
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
	struct sway_layer_surface *sway_layer_surface =
		wl_container_of(listener, sway_layer_surface, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	create_popup(wlr_popup, sway_layer_surface->scene->node);
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

	struct sway_output *output = layer_surface->output->data;

	enum zwlr_layer_shell_v1_layer layer_type = layer_surface->pending.layer;
	struct wlr_scene_node *output_layer = sway_layer_get_scene(
		output, layer_type);
	struct wlr_scene_layer_surface_v1 *scene_surface =
		wlr_scene_layer_surface_v1_create(output_layer, layer_surface);
	if (!scene_surface) {
		sway_log(SWAY_ERROR, "Could not allocate a layer_surface_v1");
		return;
	}

	struct sway_layer_surface *surface =
		sway_layer_surface_create(scene_surface);
	if (!surface) {
		wlr_layer_surface_v1_destroy(layer_surface);

		sway_log(SWAY_ERROR, "Could not allocate a sway_layer_surface");
		return;
	}

	scene_descriptor_assign(scene_surface->node,
		SWAY_SCENE_DESC_LAYER_SHELL, surface);
	if (!scene_surface->node->data) {
		// destroying the layer_surface will also destroy its corresponding
		// scene node
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	surface->output = output;

	surface->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit,
		&surface->surface_commit);
	surface->map.notify = handle_map;
	wl_signal_add(&layer_surface->events.map, &surface->map);
	surface->unmap.notify = handle_unmap;
	wl_signal_add(&layer_surface->events.unmap, &surface->unmap);
	surface->destroy.notify = handle_destroy;
	wl_signal_add(&layer_surface->events.destroy, &surface->destroy);	
	surface->new_popup.notify = handle_new_popup;
	wl_signal_add(&layer_surface->events.new_popup, &surface->new_popup);

	surface->output_destroy.notify = handle_output_destroy;
	wl_signal_add(&output->events.disable, &surface->output_destroy);

	// Temporarily set the layer's current state to pending
	// So that we can easily arrange it
	struct wlr_layer_surface_v1_state old_state = layer_surface->current;
	layer_surface->current = layer_surface->pending;
	arrange_layers(output);
	layer_surface->current = old_state;
}
