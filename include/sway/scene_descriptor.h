#ifndef _SWAY_SCENE_DESCRIPTOR_H
#define _SWAY_SCENE_DESCRIPTOR_H
#include <wlr/types/wlr_scene.h>

enum sway_scene_descriptor_type {
	SWAY_SCENE_DESC_BUFFER_TIMER,
};

bool scene_descriptor_assign(struct wlr_scene_node *node,
	enum sway_scene_descriptor_type type, void *data);

void *scene_descriptor_try_get(struct wlr_scene_node *node,
	enum sway_scene_descriptor_type type);

void scene_descriptor_destroy(struct wlr_scene_node *node,
	enum sway_scene_descriptor_type type);

#endif
