#ifndef _SWAY_LAYERS_H
#define _SWAY_LAYERS_H
#include <stdbool.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>

struct sway_layer_surface {
	struct wl_listener destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
	struct wl_listener output_destroy;
	struct wl_listener new_popup;

	bool mapped;

	struct sway_output *output;
	struct wlr_scene_layer_surface_v1 *scene;
};

struct sway_layer_popup {
	struct wlr_xdg_popup *wlr_popup;
	struct wlr_scene_node *scene;

	struct wl_listener destroy;
	struct wl_listener new_popup;
};

struct sway_output;
void arrange_layers(struct sway_output *output);

#endif
