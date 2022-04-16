#ifndef _SWAY_SCENE_DESCRIPTOR_H
#define _SWAY_SCENE_DESCRIPTOR_H
#include <wlr/types/wlr_scene.h>

enum sway_scene_descriptor_type {
	SWAY_SCENE_DESC_NON_INTERACTIVE,
	SWAY_SCENE_DESC_CONTAINER,
	SWAY_SCENE_DESC_LAYER_SHELL,
	SWAY_SCENE_DESC_LAYER_SHELL_POPUP,
	SWAY_SCENE_DESC_DRAG_ICON,
};

struct sway_scene_descriptor {
	enum sway_scene_descriptor_type type;
	void *data;

	struct wl_listener destroy;
};

void scene_descriptor_assign(struct wlr_scene_node *node,
	enum sway_scene_descriptor_type type, void *data);

#endif
