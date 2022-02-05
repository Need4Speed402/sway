#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/headless.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output_damage.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/region.h>
#include "config.h"
#include "log.h"
#include "sway/config.h"
#include "sway/desktop/transaction.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/layers.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/surface.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"

struct sway_output *output_by_name_or_id(const char *name_or_id) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		char identifier[128];
		output_get_identifier(identifier, sizeof(identifier), output);
		if (strcasecmp(identifier, name_or_id) == 0
				|| strcasecmp(output->wlr_output->name, name_or_id) == 0) {
			return output;
		}
	}
	return NULL;
}

struct sway_output *all_output_by_name_or_id(const char *name_or_id) {
	struct sway_output *output;
	wl_list_for_each(output, &root->all_outputs, link) {
		char identifier[128];
		output_get_identifier(identifier, sizeof(identifier), output);
		if (strcasecmp(identifier, name_or_id) == 0
				|| strcasecmp(output->wlr_output->name, name_or_id) == 0) {
			return output;
		}
	}
	return NULL;
}


struct sway_workspace *output_get_active_workspace(struct sway_output *output) {
	struct sway_seat *seat = input_manager_current_seat();
	struct sway_node *focus = seat_get_active_tiling_child(seat, &output->node);
	if (!focus) {
		if (!output->workspaces->length) {
			return NULL;
		}
		return output->workspaces->items[0];
	}
	return focus->sway_workspace;
}

static void handle_output_frame(struct wl_listener *listener, void *data) {
	printf("try to render frame\n");
	struct sway_output *output = wl_container_of(listener, output, frame);

	if (!wlr_scene_output_commit(output->scene_output)) {
		return;
	}

	struct timespec now = {0};
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void handle_needs_frame(struct wl_listener *listener, void *data) {
	//printf("requesting a frame to render\n");
}

static void update_output_manager_config(struct sway_server *server) {
	struct wlr_output_configuration_v1 *config =
		wlr_output_configuration_v1_create();

	struct sway_output *output;
	wl_list_for_each(output, &root->all_outputs, link) {
		if (output == root->fallback_output) {
			continue;
		}
		struct wlr_output_configuration_head_v1 *config_head =
			wlr_output_configuration_head_v1_create(config, output->wlr_output);
		struct wlr_box output_box;
		wlr_output_layout_get_box(root->output_layout,
			output->wlr_output, &output_box);
		// We mark the output enabled even if it is switched off by DPMS
		config_head->state.enabled = output->current_mode != NULL && output->enabled;
		config_head->state.mode = output->current_mode;
		if (!wlr_box_empty(&output_box)) {
			config_head->state.x = output_box.x;
			config_head->state.y = output_box.y;
		}
	}

	wlr_output_manager_v1_set_configuration(server->output_manager_v1, config);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct sway_output *output = wl_container_of(listener, output, destroy);
	struct sway_server *server = output->server;
	output_begin_destroy(output);

	if (output->enabled) {
		output_disable(output);
	}

	wl_list_remove(&output->link);

	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->commit.link);
	wl_list_remove(&output->mode.link);
	wl_list_remove(&output->present.link);

	output->wlr_output->data = NULL;
	output->wlr_output = NULL;

	transaction_commit_dirty();

	update_output_manager_config(server);
}

static void handle_mode(struct wl_listener *listener, void *data) {
	struct sway_output *output = wl_container_of(listener, output, mode);
	if (!output->enabled && !output->enabling) {
		struct output_config *oc = find_output_config(output);
		if (output->wlr_output->current_mode != NULL &&
				(!oc || oc->enabled)) {
			// We want to enable this output, but it didn't work last time,
			// possibly because we hadn't enough CRTCs. Try again now that the
			// output has a mode.
			sway_log(SWAY_DEBUG, "Output %s has gained a CRTC, "
				"trying to enable it", output->wlr_output->name);
			apply_output_config(oc, output);
		}
		return;
	}
	if (!output->enabled) {
		return;
	}
	arrange_layers(output);
	arrange_output(output);
	transaction_commit_dirty();

	update_output_manager_config(output->server);
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct sway_output *output = wl_container_of(listener, output, commit);
	struct wlr_output_event_commit *event = data;

	if (!output->enabled) {
		return;
	}

	if (event->committed & (WLR_OUTPUT_STATE_TRANSFORM | WLR_OUTPUT_STATE_SCALE)) {
		arrange_layers(output);
		arrange_output(output);
		transaction_commit_dirty();

		update_output_manager_config(output->server);
	}
}

static void handle_present(struct wl_listener *listener, void *data) {
	struct sway_output *output = wl_container_of(listener, output, present);
	struct wlr_output_event_present *output_event = data;

	if (!output->enabled || !output_event->presented) {
		return;
	}

	output->last_presentation = *output_event->when;
	output->refresh_nsec = output_event->refresh;
}

static unsigned int last_headless_num = 0;

void handle_new_output(struct wl_listener *listener, void *data) {
	struct sway_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	if (wlr_output == root->fallback_output->wlr_output) {
		return;
	}

	if (wlr_output_is_headless(wlr_output)) {
		char name[64];
		snprintf(name, sizeof(name), "HEADLESS-%u", ++last_headless_num);
		wlr_output_set_name(wlr_output, name);
	}

	sway_log(SWAY_DEBUG, "New output %p: %s (non-desktop: %d)",
			wlr_output, wlr_output->name, wlr_output->non_desktop);

	if (wlr_output->non_desktop) {
		sway_log(SWAY_DEBUG, "Not configuring non-desktop output");
		if (server->drm_lease_manager) {
			wlr_drm_lease_v1_manager_offer_output(server->drm_lease_manager,
					wlr_output);
		}
		return;
	}

	if (!wlr_output_init_render(wlr_output, server->allocator,
			server->renderer)) {
		sway_log(SWAY_ERROR, "Failed to init output render");
		return;
	}

	struct sway_output *output = output_create(wlr_output);
	if (!output) {
		return;
	}

	wlr_scene_node_reparent(output->node.scene_node, root->node.scene_node);

	output->server = server;
	output->scene_output = wlr_scene_output_create(root->root_scene, wlr_output);

	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	output->destroy.notify = handle_destroy;
	wl_signal_add(&wlr_output->events.commit, &output->commit);
	output->commit.notify = handle_commit;
	wl_signal_add(&wlr_output->events.mode, &output->mode);
	output->mode.notify = handle_mode;
	wl_signal_add(&wlr_output->events.present, &output->present);
	output->present.notify = handle_present;

	output->frame.notify = handle_output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->needs_frame.notify = handle_needs_frame;
	wl_signal_add(&wlr_output->events.needs_frame, &output->needs_frame);

	/*output->repaint_timer = wl_event_loop_add_timer(server->wl_event_loop,
		output_repaint_timer_handler, output);
	*/

	struct output_config *oc = find_output_config(output);
	apply_output_config(oc, output);
	free_output_config(oc);

	transaction_commit_dirty();

	update_output_manager_config(server);
}

void handle_output_layout_change(struct wl_listener *listener,
		void *data) {
	struct sway_server *server =
		wl_container_of(listener, server, output_layout_change);
	update_output_manager_config(server);
}

static void output_manager_apply(struct sway_server *server,
		struct wlr_output_configuration_v1 *config, bool test_only) {
	// TODO: perform atomic tests on the whole backend atomically

	struct wlr_output_configuration_head_v1 *config_head;
	// First disable outputs we need to disable
	bool ok = true;
	wl_list_for_each(config_head, &config->heads, link) {
		struct wlr_output *wlr_output = config_head->state.output;
		struct sway_output *output = wlr_output->data;
		if (!output->enabled || config_head->state.enabled) {
			continue;
		}
		struct output_config *oc = new_output_config(output->wlr_output->name);
		oc->enabled = false;

		if (test_only) {
			ok &= test_output_config(oc, output);
		} else {
			oc = store_output_config(oc);
			ok &= apply_output_config(oc, output);
		}
	}

	// Then enable outputs that need to
	wl_list_for_each(config_head, &config->heads, link) {
		struct wlr_output *wlr_output = config_head->state.output;
		struct sway_output *output = wlr_output->data;
		if (!config_head->state.enabled) {
			continue;
		}
		struct output_config *oc = new_output_config(output->wlr_output->name);
		oc->enabled = true;
		if (config_head->state.mode != NULL) {
			struct wlr_output_mode *mode = config_head->state.mode;
			oc->width = mode->width;
			oc->height = mode->height;
			oc->refresh_rate = mode->refresh / 1000.f;
		} else {
			oc->width = config_head->state.custom_mode.width;
			oc->height = config_head->state.custom_mode.height;
			oc->refresh_rate =
				config_head->state.custom_mode.refresh / 1000.f;
		}
		oc->x = config_head->state.x;
		oc->y = config_head->state.y;
		oc->transform = config_head->state.transform;
		oc->scale = config_head->state.scale;

		if (test_only) {
			ok &= test_output_config(oc, output);
		} else {
			oc = store_output_config(oc);
			ok &= apply_output_config(oc, output);
		}
	}

	if (ok) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);

	if (!test_only) {
		update_output_manager_config(server);
	}
}

void handle_output_manager_apply(struct wl_listener *listener, void *data) {
	struct sway_server *server =
		wl_container_of(listener, server, output_manager_apply);
	struct wlr_output_configuration_v1 *config = data;

	output_manager_apply(server, config, false);
}

void handle_output_manager_test(struct wl_listener *listener, void *data) {
	struct sway_server *server =
		wl_container_of(listener, server, output_manager_test);
	struct wlr_output_configuration_v1 *config = data;

	output_manager_apply(server, config, true);
}

void handle_output_power_manager_set_mode(struct wl_listener *listener,
		void *data) {
	struct wlr_output_power_v1_set_mode_event *event = data;
	struct sway_output *output = event->output->data;

	struct output_config *oc = new_output_config(output->wlr_output->name);
	switch (event->mode) {
	case ZWLR_OUTPUT_POWER_V1_MODE_OFF:
		oc->dpms_state = DPMS_OFF;
		break;
	case ZWLR_OUTPUT_POWER_V1_MODE_ON:
		oc->dpms_state = DPMS_ON;
		break;
	}
	oc = store_output_config(oc);
	apply_output_config(oc, output);
}
