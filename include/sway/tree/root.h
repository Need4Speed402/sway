#ifndef _SWAY_ROOT_H
#define _SWAY_ROOT_H
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include "sway/tree/container.h"
#include "sway/tree/node.h"
#include "config.h"
#include "list.h"

extern struct sway_root *root;

struct sway_root {
	struct sway_node node;
	struct wlr_scene *root_scene;

	// since wlr_scene nodes can't be orphaned and must always
	// have a parent, use this staging scene_tree so that a
	// node always have a valid parent. Nothing in this
	// staging node will be visible.
	struct wlr_scene_tree *staging;

	struct wlr_output_layout *output_layout;

	struct wl_listener output_layout_change;
#if HAVE_XWAYLAND
	struct wl_list xwayland_unmanaged; // sway_xwayland_unmanaged::link
#endif
	struct wl_list drag_icons; // sway_drag_icon::link

	// Includes disabled outputs
	struct wl_list all_outputs; // sway_output::link

	double x, y;
	double width, height;

	list_t *outputs; // struct sway_output
	list_t *scratchpad; // struct sway_container

	// For when there's no connected outputs
	struct sway_output *fallback_output;

	struct sway_container *fullscreen_global;

	struct {
		struct wl_signal new_node;
	} events;
};

struct sway_root *root_create(void);

void root_destroy(struct sway_root *root);

/**
 * Move a container to the scratchpad.
 * If a workspace is passed, the container is assumed to have been in
 * the scratchpad before and is shown on the workspace.
 * The ws parameter can safely be NULL.
 */
void root_scratchpad_add_container(struct sway_container *con,
   struct sway_workspace *ws);

/**
 * Remove a container from the scratchpad.
 */
void root_scratchpad_remove_container(struct sway_container *con);

/**
 * Show a single scratchpad container.
 */
void root_scratchpad_show(struct sway_container *con);

/**
 * Hide a single scratchpad container.
 */
void root_scratchpad_hide(struct sway_container *con);

struct sway_workspace *root_workspace_for_pid(pid_t pid);

void root_record_workspace_pid(pid_t pid);

void root_remove_workspace_pid(pid_t pid);

void root_for_each_workspace(void (*f)(struct sway_workspace *ws, void *data),
		void *data);

void root_for_each_container(void (*f)(struct sway_container *con, void *data),
		void *data);

struct sway_output *root_find_output(
		bool (*test)(struct sway_output *output, void *data), void *data);

struct sway_workspace *root_find_workspace(
		bool (*test)(struct sway_workspace *ws, void *data), void *data);

struct sway_container *root_find_container(
		bool (*test)(struct sway_container *con, void *data), void *data);

void root_get_box(struct sway_root *root, struct wlr_box *box);

void root_rename_pid_workspaces(const char *old_name, const char *new_name);

#endif
