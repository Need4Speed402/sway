#ifndef _SWAY_BUFFER_H
#define _SWAY_BUFFER_H
#include <wlr/interfaces/wlr_buffer.h>

struct sway_buffer {
	struct wlr_buffer base;
	void *data;
	uint32_t format;
	size_t stride;

	uint32_t width, height, scale;
};

struct sway_buffer *sway_buffer_create(uint32_t width, uint32_t height,
		uint32_t scale, uint32_t format);

void sway_buffer_destroy(struct sway_buffer *buffer);

struct sway_buffer *sway_buffer_from_wlr_buffer(struct wlr_buffer *wlr_buffer);

#endif
