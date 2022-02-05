/*
 * Copyright (c) 2022 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <libdrm/drm_fourcc.h>
#include <aml.h>
#include <neatvnc.h>

#include "screencopy-interface.h"
#include "screencopy-unstable-v1.h"
#include "buffer.h"
#include "shm.h"
#include "time-util.h"
#include "usdt.h"
#include "pixels.h"
#include "config.h"
#include "logging.h"

struct ext_screencopy {
	struct screencopy parent;
	struct zext_screencopy_manager_v1* manager;
	struct wl_output* wl_output;
	struct zext_screencopy_surface_v1* surface;
	bool render_cursors;
	struct wv_buffer_pool* pool;
	struct wv_buffer* buffer;
	struct wv_buffer_pool* cursor_pool;
	struct wv_buffer* cursor_buffer;
	bool have_buffer_info;
	bool should_start;
	bool shall_be_immediate;
	bool have_cursor;

	struct {
		uint32_t wl_shm_width, wl_shm_height, wl_shm_stride, wl_shm_format;

		bool have_linux_dmabuf;
		uint32_t dmabuf_width, dmabuf_height, dmabuf_format;
	} output, cursor;


	void* userdata;
	screencopy_done_fn on_done;
};

struct screencopy_impl ext_screencopy_impl;

static struct zext_screencopy_surface_v1_listener surface_listener;

static int ext_screencopy_init_surface(struct ext_screencopy* self)
{
	if (self->surface)
		zext_screencopy_surface_v1_destroy(self->surface);

	self->surface = zext_screencopy_manager_v1_capture_output(self->manager,
			self->wl_output);
	if (!self->surface)
		return -1;

	zext_screencopy_surface_v1_add_listener(self->surface,
			&surface_listener, self);

	return 0;
}

// TODO: Throttle capturing to max_fps
static void ext_screencopy_schedule_capture(struct ext_screencopy* self,
		bool immediate)
{
	self->buffer = wv_buffer_pool_acquire(self->pool);
	self->buffer->domain = WV_BUFFER_DOMAIN_OUTPUT;

	zext_screencopy_surface_v1_attach_buffer(self->surface,
			self->buffer->wl_buffer);

	int n_rects = 0;
	struct pixman_box16* rects =
		pixman_region_rectangles(&self->buffer->buffer_damage, &n_rects);

	for (int i = 0; i < n_rects; ++i) {
		uint32_t x = rects[i].x1;
		uint32_t y = rects[i].y1;
		uint32_t width = rects[i].x2 - x;
		uint32_t height = rects[i].y2 - y;

		zext_screencopy_surface_v1_damage_buffer(self->surface, x, y,
				width, height);
	}

	uint32_t flags = ZEXT_SCREENCOPY_SURFACE_V1_OPTIONS_NONE;

	if (immediate)
		flags |= ZEXT_SCREENCOPY_SURFACE_V1_OPTIONS_IMMEDIATE;

	if (self->render_cursors)
		flags |= ZEXT_SCREENCOPY_SURFACE_V1_OPTIONS_RENDER_CURSORS;

	if (self->have_cursor) {
		self->cursor_buffer = wv_buffer_pool_acquire(self->cursor_pool);
		self->cursor_buffer->domain = WV_BUFFER_DOMAIN_CURSOR;

		if (pixman_region_not_empty(&self->cursor_buffer->buffer_damage))
			zext_screencopy_surface_v1_damage_cursor_buffer(
					self->surface, "default");

		zext_screencopy_surface_v1_attach_cursor_buffer(self->surface,
				self->cursor_buffer->wl_buffer, "default");
	}

	zext_screencopy_surface_v1_commit(self->surface, flags);

	log_debug("Committed buffer%s: %p\n", immediate ? " immediately" : "",
			self->buffer);
}

static void surface_handle_reconfig(void *data,
		struct zext_screencopy_surface_v1 *surface)
{
	struct ext_screencopy* self = data;

	self->have_buffer_info = false;
	self->output.have_linux_dmabuf = false;
}

static void surface_handle_buffer_info(void *data,
		struct zext_screencopy_surface_v1 *surface,
		enum zext_screencopy_surface_v1_buffer_type type,
		uint32_t format, uint32_t width, uint32_t height,
		uint32_t stride)
{
	struct ext_screencopy* self = data;

	switch (type) {
	case ZEXT_SCREENCOPY_SURFACE_V1_BUFFER_TYPE_WL_SHM:
		self->output.wl_shm_format = format;
		self->output.wl_shm_width = width;
		self->output.wl_shm_height = height;
		self->output.wl_shm_stride = stride;
		log_debug("Got shm buffer\n");
		break;
#ifdef ENABLE_SCREENCOPY_DMABUF
	case ZEXT_SCREENCOPY_SURFACE_V1_BUFFER_TYPE_DMABUF:
		self->output.dmabuf_format = format;
		self->output.dmabuf_width = width;
		self->output.dmabuf_height = height;
		log_debug("Got dmabuf\n");
		break;
#endif
	}
}

static void surface_handle_cursor_buffer_info(void *data,
		struct zext_screencopy_surface_v1 *surface, const char *name,
		enum zext_screencopy_surface_v1_buffer_type type,
		uint32_t format, uint32_t width, uint32_t height,
		uint32_t stride)
{
	struct ext_screencopy* self = data;

	switch (type) {
	case ZEXT_SCREENCOPY_SURFACE_V1_BUFFER_TYPE_WL_SHM:
		self->cursor.wl_shm_format = format;
		self->cursor.wl_shm_width = width;
		self->cursor.wl_shm_height = height;
		self->cursor.wl_shm_stride = stride;
		log_debug("Got shm buffer\n");
		break;
#ifdef ENABLE_SCREENCOPY_DMABUF
	case ZEXT_SCREENCOPY_SURFACE_V1_BUFFER_TYPE_DMABUF:
		self->cursor.dmabuf_format = format;
		self->cursor.dmabuf_width = width;
		self->cursor.dmabuf_height = height;
		log_debug("Got dmabuf\n");
		break;
#endif
	}
}

static void surface_handle_init_done(void *data,
		struct zext_screencopy_surface_v1 *surface)
{
	struct ext_screencopy* self = data;
	uint32_t width, height, stride, format;
	enum wv_buffer_type type = WV_BUFFER_UNSPEC;

#ifdef ENABLE_SCREENCOPY_DMABUF
	if (self->output.have_linux_dmabuf) {
		format = self->output.dmabuf_format;
		width = self->output.dmabuf_width;
		height = self->output.dmabuf_height;
		stride = 0;
		type = WV_BUFFER_DMABUF;
	} else
#endif
	{
		format = self->output.wl_shm_format;
		width = self->output.wl_shm_width;
		height = self->output.wl_shm_height;
		stride = self->output.wl_shm_stride;
		type = WV_BUFFER_SHM;
	}

	wv_buffer_pool_resize(self->pool, type, width, height, stride, format);

#ifdef ENABLE_SCREENCOPY_DMABUF
	if (self->cursor.have_linux_dmabuf) {
		format = self->cursor.dmabuf_format;
		width = self->cursor.dmabuf_width;
		height = self->cursor.dmabuf_height;
		stride = 0;
		type = WV_BUFFER_DMABUF;
	} else
#endif
	{
		format = self->cursor.wl_shm_format;
		width = self->cursor.wl_shm_width;
		height = self->cursor.wl_shm_height;
		stride = self->cursor.wl_shm_stride;
		type = WV_BUFFER_SHM;
	}

	wv_buffer_pool_resize(self->cursor_pool, type, width, height, stride,
			format);

	if (self->should_start) {
		ext_screencopy_schedule_capture(self, self->shall_be_immediate);

		self->should_start = false;
		self->shall_be_immediate = false;
	}

	self->have_buffer_info = true;

	log_debug("Init done\n");
}

static void surface_handle_transform(void *data,
		struct zext_screencopy_surface_v1 *surface,
		int32_t transform)
{
	struct ext_screencopy* self = data;

	assert(self->buffer);

	// TODO: Tell main.c not to override this transform
	nvnc_fb_set_transform(self->buffer->nvnc_fb, transform);
}

static void surface_handle_ready(void *data,
		struct zext_screencopy_surface_v1 *surface)
{
	struct ext_screencopy* self = data;

	log_debug("Ready!\n");

	assert(self->buffer);

	wv_buffer_registry_damage_all(&self->buffer->frame_damage,
			WV_BUFFER_DOMAIN_OUTPUT);
	pixman_region_clear(&self->buffer->buffer_damage);

	struct wv_buffer* buffer = self->buffer;
	self->buffer = NULL;

	self->on_done(SCREENCOPY_DONE, buffer, self->userdata);
}

static void surface_handle_failed(void *data,
		struct zext_screencopy_surface_v1 *surface,
		enum zext_screencopy_surface_v1_failure_reason reason)
{
	struct ext_screencopy* self = data;

	log_debug("Failed!\n");

	assert(self->buffer);

	wv_buffer_pool_release(self->pool, self->buffer);
	self->buffer = NULL;

	if (reason == ZEXT_SCREENCOPY_SURFACE_V1_FAILURE_REASON_INVALID_BUFFER)
		ext_screencopy_init_surface(self);

	self->on_done(SCREENCOPY_FAILED, NULL, self->userdata);
}

static void surface_handle_damage(void *data,
		struct zext_screencopy_surface_v1 *surface,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	struct ext_screencopy* self = data;

	wv_buffer_damage_rect(self->buffer, x, y, width, height);
}

static void surface_handle_cursor_info(void *data,
		struct zext_screencopy_surface_v1 *surface, const char *name,
		int has_damage, int32_t pos_x, int32_t pos_y,
		int32_t hotspot_x, int32_t hotspot_y)
{
	struct ext_screencopy* self = data;

	if (has_damage)
		wv_buffer_damage_whole(self->cursor_buffer);

	wv_buffer_registry_damage_all(&self->cursor_buffer->frame_damage,
			WV_BUFFER_DOMAIN_CURSOR);
	pixman_region_clear(&self->cursor_buffer->buffer_damage);

	// TODO: Implement

	// TODO: Remove this after implementing:
	wv_buffer_pool_release(self->cursor_pool, self->cursor_buffer);
}

static void surface_handle_cursor_enter(void *data,
		struct zext_screencopy_surface_v1 *surface, const char *name)
{
	struct ext_screencopy* self = data;

	self->have_cursor = true;
}

static void surface_handle_cursor_leave(void *data,
		struct zext_screencopy_surface_v1 *surface, const char *name)
{
	struct ext_screencopy* self = data;

	self->have_cursor = false;
}

static void surface_handle_commit_time(void *data,
		struct zext_screencopy_surface_v1 *surface,
		uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
	// TODO
}

static struct zext_screencopy_surface_v1_listener surface_listener = {
	.reconfig = surface_handle_reconfig,
	.buffer_info = surface_handle_buffer_info,
	.cursor_buffer_info = surface_handle_cursor_buffer_info,
	.cursor_enter = surface_handle_cursor_enter,
	.cursor_leave = surface_handle_cursor_leave,
	.init_done = surface_handle_init_done,
	.damage = surface_handle_damage,
	.cursor_info = surface_handle_cursor_info,
	.commit_time = surface_handle_commit_time,
	.transform = surface_handle_transform,
	.ready = surface_handle_ready,
	.failed = surface_handle_failed,
};

static int ext_screencopy_start(struct screencopy* ptr, bool immediate)
{
	struct ext_screencopy* self = (struct ext_screencopy*)ptr;

	if (!self->have_buffer_info) {
		self->should_start = true;
		self->shall_be_immediate = immediate;
	} else {
		ext_screencopy_schedule_capture(self, immediate);
	}

	return 0;
}

static void ext_screencopy_stop(struct screencopy* self)
{
	// Nothing to stop?
}

static struct screencopy* ext_screencopy_create(void* manager,
		struct wl_output* output, bool render_cursor, 
		screencopy_done_fn on_done, void* userdata)
{
	struct ext_screencopy* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->parent.impl = &ext_screencopy_impl;
	self->parent.rate_limit = 30;

	self->manager = manager;
	self->wl_output = output;
	self->on_done = on_done;
	self->userdata = userdata;

	self->pool = wv_buffer_pool_create(0, 0, 0, 0, 0);
	if (!self->pool)
		goto failure;

	self->cursor_pool = wv_buffer_pool_create(0, 0, 0, 0, 0);
	if (!self->pool)
		goto cursor_failure;

	if (ext_screencopy_init_surface(self) < 0)
		goto surface_failure;

	return (struct screencopy*)self;

surface_failure:
	wv_buffer_pool_destroy(self->cursor_pool);
cursor_failure:
	wv_buffer_pool_destroy(self->pool);
failure:
	free(self);
	return NULL;
}

void ext_screencopy_destroy(struct screencopy* ptr)
{
	struct ext_screencopy* self = (struct ext_screencopy*)ptr;

	if (self->surface)
		zext_screencopy_surface_v1_destroy(self->surface);
	if (self->buffer)
		wv_buffer_pool_release(self->pool, self->buffer);
	free(self);
}

struct screencopy_impl ext_screencopy_impl = {
	.create = ext_screencopy_create,
	.destroy = ext_screencopy_destroy,
	.start = ext_screencopy_start,
	.stop = ext_screencopy_stop,
};
