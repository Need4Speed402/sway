#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <wlr/types/wlr_scene.h>
#include "log.h"
#include "sway/input/keyboard.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include "sway/server.h"

struct sway_session_lock {
	struct wl_list outputs; // struct sway_session_lock_output

	struct wlr_session_lock_v1 *lock;
	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
};

struct sway_session_lock_output {
	struct wlr_scene_node *node;
	struct wlr_scene_rect *background;

	struct sway_output *output;
	struct wl_list surfaces; // struct sway_session_lock_surface

	struct wl_list link; // sway_session_lock::outputs
	bool abandoned;

	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener mode;
};

struct sway_session_lock_surface {
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct sway_session_lock_output *output;
	struct wlr_surface *surface;

	struct wl_list link; //sway_session_lock_output::surfaces

	struct wl_listener destroy;
	struct wl_listener map;
};

static void handle_surface_map(struct wl_listener *listener, void *data) {
	struct sway_session_lock_surface *surf = wl_container_of(listener, surf, map);
	sway_force_focus(surf->surface);
}

static void lock_surface_reconfigure(struct sway_session_lock_surface *surf) {
	int width = surf->output->output->width;
	int height = surf->output->output->height;

	wlr_session_lock_surface_v1_configure(surf->lock_surface, width, height);
}

static void session_lock_surface_destroy(struct sway_session_lock_surface *surf) {
	if (!surf) {
		return;
	}

	wl_list_remove(&surf->destroy.link);
	wl_list_remove(&surf->map.link);
	wl_list_remove(&surf->link);
	free(surf);
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct sway_session_lock_surface *surf = wl_container_of(listener, surf, destroy);
	session_lock_surface_destroy(surf);
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct sway_session_lock *lock = wl_container_of(listener, lock, new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	struct sway_session_lock_surface *surf =
		calloc(1, sizeof(struct sway_session_lock_surface));
	if (surf == NULL) {
		return;
	}

	sway_log(SWAY_DEBUG, "new lock layer surface");
	struct sway_output *output = lock_surface->output->data;

	struct sway_session_lock_output *current_lock_output, *lock_output = NULL;
	wl_list_for_each(current_lock_output, &lock->outputs, link) {
		if (current_lock_output->output == output) {
			lock_output = current_lock_output;
			break;
		}
	}
	assert(lock_output);

	wlr_scene_subsurface_tree_create(lock_output->node, lock_surface->surface);

	surf->lock_surface = lock_surface;
	surf->output = lock_output;
	surf->surface = lock_surface->surface;

	wl_list_insert(&lock_output->surfaces, &surf->link);

	surf->destroy.notify = handle_surface_destroy;
	wl_signal_add(&lock_surface->events.destroy, &surf->destroy);
	surf->map.notify = handle_surface_map;
	wl_signal_add(&lock_surface->events.map, &surf->map);

	lock_surface_reconfigure(surf);
}

static void lock_output_reconfigure(struct sway_session_lock_output *output) {
	int width = output->output->width;
	int height = output->output->height;

	struct sway_session_lock_surface *surf, *tmp_surf;
	wl_list_for_each_safe(surf, tmp_surf, &output->surfaces, link) {
		lock_surface_reconfigure(surf);
	}

	wlr_scene_rect_set_size(output->background, width, height);
}

static void sway_session_lock_output_destroy(struct sway_session_lock_output *output) {
	struct sway_session_lock_surface *surf, *tmp_surf;
	wl_list_for_each_safe(surf, tmp_surf, &output->surfaces, link) {
		session_lock_surface_destroy(surf);
	}

	//wlr_scene_node_destroy(output->node);

	wl_list_remove(&output->mode.link);
	wl_list_remove(&output->commit.link);
	wl_list_remove(&output->destroy.link);

	if (!output->abandoned) {
		// if the lock is abandoned, that means that there is no sway_session_lock
		// object. Don't remove the output from an invalid array
		wl_list_remove(&output->link);
	}

	free(output);
}

static void lock_output_handle_destroy(struct wl_listener *listener, void *data) {
	struct sway_session_lock_output *output =
		wl_container_of(listener, output, destroy);
	sway_session_lock_output_destroy(output);
}

static void lock_output_handle_mode(struct wl_listener *listener, void *data) {
	struct sway_session_lock_output *output =
		wl_container_of(listener, output, mode);
	lock_output_reconfigure(output);
}

static void lock_output_handle_commit(struct wl_listener *listener, void *data) {
	struct wlr_output_event_commit *event = data;
	struct sway_session_lock_output *output =
		wl_container_of(listener, output, commit);
	if (event->committed & (
			WLR_OUTPUT_STATE_MODE |
			WLR_OUTPUT_STATE_SCALE |
			WLR_OUTPUT_STATE_TRANSFORM)) {
		lock_output_reconfigure(output);
	}
}

static struct sway_session_lock_output *session_lock_output_create(
		struct sway_output *output) {
	struct sway_session_lock_output *lock_output =
		calloc(1, sizeof(struct sway_session_lock_output));
	if (!lock_output) {
		sway_log(SWAY_ERROR, "failed to allocate a session lock output");
		return NULL;
	}

	struct wlr_scene_tree *tree = wlr_scene_tree_create(output->layers.session_lock);
	if (!tree) {
		sway_log(SWAY_ERROR, "failed to allocate a session lock output scene tree");
		free(lock_output);
		return NULL;
	}

	struct wlr_scene_rect *background = wlr_scene_rect_create(&tree->node,
		0, 0, (float[4]){ 1.f, 0.f, 0.f, 1.f });

	lock_output->output = output;
	lock_output->node = &tree->node;
	lock_output->background = background;
	wl_list_init(&lock_output->surfaces);

	lock_output->destroy.notify = lock_output_handle_destroy;
	wl_signal_add(&output->wlr_output->events.destroy, &lock_output->destroy);
	lock_output->commit.notify = lock_output_handle_commit;
	wl_signal_add(&output->wlr_output->events.commit, &lock_output->commit);
	lock_output->mode.notify = lock_output_handle_mode;
	wl_signal_add(&output->wlr_output->events.mode, &lock_output->mode);

	lock_output_reconfigure(lock_output);

	return lock_output;
}

static void sway_session_lock_destroy(struct sway_session_lock* lock) {
	if (!lock) {
		return;
	}

	if (server.session_lock.lock == lock) {
		server.session_lock.lock = NULL;
	}

	wl_list_remove(&lock->destroy.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->new_surface.link);

	free(lock);
}

static void handle_unlock(struct wl_listener *listener, void *data) {
	struct sway_session_lock *lock = wl_container_of(listener, lock, unlock);
	sway_log(SWAY_DEBUG, "session unlocked");

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_set_exclusive_client(seat, NULL);
		// copied from seat_set_focus_layer -- deduplicate?
		struct sway_node *previous = seat_get_focus_inactive(seat, &root->node);
		if (previous) {
			// Hack to get seat to re-focus the return value of get_focus
			seat_set_focus(seat, NULL);
			seat_set_focus(seat, previous);
		}
	}

	struct sway_session_lock_output *lock_output, *tmp_lock_output;
	wl_list_for_each_safe(lock_output, tmp_lock_output, &lock->outputs, link) {
		sway_session_lock_output_destroy(lock_output);
	}

	server.session_lock.locked = false;
	sway_session_lock_destroy(lock);
}

static void handle_abandon(struct wl_listener *listener, void *data) {
	struct sway_session_lock *lock = wl_container_of(listener, lock, destroy);
	sway_log(SWAY_INFO, "session lock abandoned");

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat->exclusive_client = NULL;
	}

	struct sway_session_lock_output *lock_output;
	wl_list_for_each(lock_output, &lock->outputs, link) {
		lock_output->abandoned = true;
	}

	sway_session_lock_destroy(lock);
}

static void handle_session_lock(struct wl_listener *listener, void *data) {
	struct wlr_session_lock_v1 *lock = data;
	struct wl_client *client = wl_resource_get_client(lock->resource);

	struct sway_session_lock *sway_lock =
		calloc(1, sizeof(struct sway_session_lock));
	if (!sway_lock) {
		sway_log(SWAY_ERROR, "failed to allocate a session lock object");
		wlr_session_lock_v1_destroy(lock);
		return;
	}

	wl_list_init(&sway_lock->outputs);

	sway_log(SWAY_DEBUG, "session locked");

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_set_exclusive_client(seat, client);
	}

	for (int i = 0; i < root->outputs->length; i++) {
		struct sway_output *output = root->outputs->items[i];
		sway_session_lock_add_output(sway_lock, output);
	}

	sway_lock->new_surface.notify = handle_new_surface;
	wl_signal_add(&lock->events.new_surface, &sway_lock->new_surface);
	sway_lock->unlock.notify = handle_unlock;
	wl_signal_add(&lock->events.unlock, &sway_lock->unlock);
	sway_lock->destroy.notify = handle_abandon;
	wl_signal_add(&lock->events.destroy, &sway_lock->destroy);

	wlr_session_lock_v1_send_locked(lock);
	server.session_lock.lock = sway_lock;
	server.session_lock.locked = true;
}

static void handle_session_lock_destroy(struct wl_listener *listener, void *data) {
	assert(server.session_lock.lock == NULL);
	wl_list_remove(&server.session_lock.new_lock.link);
	wl_list_remove(&server.session_lock.manager_destroy.link);

	server.session_lock.manager = NULL;
}

void sway_session_lock_add_output(struct sway_session_lock *lock,
		struct sway_output *output) {
	struct sway_session_lock_output *lock_output =
		session_lock_output_create(output);

	// if we run out of memory, we're SOL. Aborting is also bad because
	// that could kick the user out into a logged in shell
	if (lock_output) {
		wl_list_insert(&lock->outputs, &lock_output->link);
	}
}

void sway_session_lock_init(void) {
	server.session_lock.manager = wlr_session_lock_manager_v1_create(server.wl_display);

	server.session_lock.new_lock.notify = handle_session_lock;
	server.session_lock.manager_destroy.notify = handle_session_lock_destroy;
	wl_signal_add(&server.session_lock.manager->events.new_lock,
		&server.session_lock.new_lock);
	wl_signal_add(&server.session_lock.manager->events.destroy,
		&server.session_lock.manager_destroy);
}
