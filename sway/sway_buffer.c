#include <stdlib.h>
#include "sway/sway_buffer.h"

void sway_buffer_destroy(struct sway_buffer *buffer) {
	free(buffer->data);
	free(buffer);
}

static void handle_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct sway_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	sway_buffer_destroy(buffer);
}

static bool handle_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct sway_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	*data = buffer->data;
	*format = buffer->format;
	*stride = buffer->stride;
	return true;
}

static void handle_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	// This space is intentionally left blank
}

static const struct wlr_buffer_impl sway_buffer_impl = {
	.destroy = handle_buffer_destroy,
	.begin_data_ptr_access = handle_begin_data_ptr_access,
	.end_data_ptr_access = handle_end_data_ptr_access,
};

struct sway_buffer *sway_buffer_create(uint32_t width, uint32_t height,
		uint32_t scale, uint32_t format) {
	struct sway_buffer *buffer = calloc(1, sizeof(struct sway_buffer));
	if (buffer == NULL) {
		return NULL;
	}

	wlr_buffer_init(&buffer->base, &sway_buffer_impl,
		width * scale, height * scale);
	buffer->format = format;
	buffer->stride = 4 * width * scale;
	buffer->width = width;
	buffer->height = height;
	buffer->scale = scale;

	buffer->data = malloc(buffer->stride * height * scale);
	if (buffer->data == NULL) {
		free(buffer);
		return NULL;
	}

	return buffer;
}

struct sway_buffer *sway_buffer_from_wlr_buffer(struct wlr_buffer *wlr_buffer) {
	if (!wlr_buffer) {
		return NULL;
	}

	struct sway_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	return buffer;
}
