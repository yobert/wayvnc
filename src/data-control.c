/*
 * Copyright (c) 2020 Scott Moreau
 * Copyright (c) 2020 Andri Yngvason
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <aml.h>

#include "logging.h"
#include "data-control.h"

struct receive_context {
	struct data_control* data_control;
	struct zwlr_data_control_offer_v1* offer;
	int fd;
	FILE* mem_fp;
	size_t mem_size;
	char* mem_data;
};

static void destroy_receive_context(void* raw_ctx)
{
	struct receive_context* ctx = raw_ctx;
	int fd = ctx->fd;

	if (ctx->mem_fp)
		fclose(ctx->mem_fp);
	free(ctx->mem_data);
	zwlr_data_control_offer_v1_destroy(ctx->offer);
	close(fd);
	free(ctx);
}

static void on_receive(void* handler)
{
	struct receive_context* ctx = aml_get_userdata(handler);
	int fd = aml_get_fd(handler);
	assert(ctx->fd == fd);

	char buf[4096];

	ssize_t ret = read(fd, &buf, sizeof(buf));
	if (ret > 0) {
		fwrite(&buf, 1, ret, ctx->mem_fp);
		return;
	}

	fclose(ctx->mem_fp);
	ctx->mem_fp = NULL;

	if (ctx->mem_size)
		nvnc_send_cut_text(ctx->data_control->server, ctx->mem_data,
				ctx->mem_size);

	aml_stop(aml_get_default(), handler);
}

static void receive_data(void* data,
	struct zwlr_data_control_offer_v1* offer)
{
	struct data_control* self = data;
	int pipe_fd[2];

	if (pipe(pipe_fd) == -1) {
		log_error("pipe() failed: %m\n");
		return;
	}

	struct receive_context* ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		log_error("OOM\n");
		close(pipe_fd[0]);
		close(pipe_fd[1]);
		return;
	}

	zwlr_data_control_offer_v1_receive(offer, self->mime_type, pipe_fd[1]);
	wl_display_flush(self->wl_display);
	close(pipe_fd[1]);

	ctx->fd = pipe_fd[0];
	ctx->data_control = self;
	ctx->offer = offer;
	ctx->mem_fp = open_memstream(&ctx->mem_data, &ctx->mem_size);
	if (!ctx->mem_fp) {
		close(ctx->fd);
		free(ctx);
		log_error("open_memstream() failed: %m\n");
		return;
	}

	struct aml_handler* handler = aml_handler_new(ctx->fd, on_receive,
			ctx, destroy_receive_context);
	if (!handler) {
		close(ctx->fd);
		free(ctx);
		return;
	}

	aml_start(aml_get_default(), handler);
	aml_unref(handler);
}

static void data_control_offer(void* data,
	struct zwlr_data_control_offer_v1* zwlr_data_control_offer_v1,
	const char* mime_type)
{
	struct data_control* self = data;

	if (self->offer)
		return;
	if (strcmp(mime_type, self->mime_type) != 0) {
		return;
	}

	self->offer = zwlr_data_control_offer_v1;
}

struct zwlr_data_control_offer_v1_listener data_control_offer_listener = {
	data_control_offer
};

static void data_control_device_offer(void* data,
	struct zwlr_data_control_device_v1* zwlr_data_control_device_v1,
	struct zwlr_data_control_offer_v1* id)
{
	if (!id)
		return;

	zwlr_data_control_offer_v1_add_listener(id, &data_control_offer_listener, data);
}

static void data_control_device_selection(void* data,
	struct zwlr_data_control_device_v1* zwlr_data_control_device_v1,
	struct zwlr_data_control_offer_v1* id)
{
	struct data_control* self = data;
	if (id && self->offer == id) {
		receive_data(data, id);
		self->offer = NULL;
	}
}

static void data_control_device_finished(void* data,
	struct zwlr_data_control_device_v1* zwlr_data_control_device_v1)
{
	zwlr_data_control_device_v1_destroy(zwlr_data_control_device_v1);
}

static void data_control_device_primary_selection(void* data,
	struct zwlr_data_control_device_v1* zwlr_data_control_device_v1,
	struct zwlr_data_control_offer_v1* id)
{
	struct data_control* self = data;
	if (id && self->offer == id) {
		receive_data(data, id);
		self->offer = NULL;
		return;
	}
}

static struct zwlr_data_control_device_v1_listener data_control_device_listener = {
	.data_offer = data_control_device_offer,
	.selection = data_control_device_selection,
	.finished = data_control_device_finished,
	.primary_selection = data_control_device_primary_selection
};

static void
data_control_source_send(void* data,
	struct zwlr_data_control_source_v1* zwlr_data_control_source_v1,
	const char* mime_type,
	int32_t fd)
{
	struct data_control* self = data;
	char* d = self->cb_data;
	size_t len = self->cb_len;
	int ret;

	assert(d);

	ret = write(fd, d, len);

	if (ret < (int)len)
		log_error("write from clipboard incomplete\n");

	close(fd);
}

static void data_control_source_cancelled(void* data,
	struct zwlr_data_control_source_v1* zwlr_data_control_source_v1)
{
	struct data_control* self = data;

	if (self->selection == zwlr_data_control_source_v1) {
		self->selection = NULL;
	}
	if (self->primary_selection == zwlr_data_control_source_v1) {
		self->primary_selection = NULL;
	}
	zwlr_data_control_source_v1_destroy(zwlr_data_control_source_v1);
}

struct zwlr_data_control_source_v1_listener data_control_source_listener = {
	.send = data_control_source_send,
	.cancelled = data_control_source_cancelled
};

static struct zwlr_data_control_source_v1* set_selection(struct data_control* self, bool primary) {
	if(!self->manager) {
		return NULL;
	}

	struct zwlr_data_control_source_v1* selection;
	selection = zwlr_data_control_manager_v1_create_data_source(self->manager);
	if (selection == NULL) {
		log_error("zwlr_data_control_manager_v1_create_data_source() failed\n");
		free(self->cb_data);
		self->cb_data = NULL;
		return NULL;
	}

	zwlr_data_control_source_v1_add_listener(selection, &data_control_source_listener, self);
	zwlr_data_control_source_v1_offer(selection, self->mime_type);

	if (primary)
		zwlr_data_control_device_v1_set_primary_selection(self->device, selection);
	else
		zwlr_data_control_device_v1_set_selection(self->device, selection);

	return selection;
}

void data_control_init(struct data_control* self, struct wl_display* wl_display, struct nvnc* server, struct wl_seat* seat)
{
	self->wl_display = wl_display;
	self->server = server;
	self->device = zwlr_data_control_manager_v1_get_data_device(self->manager, seat);
	zwlr_data_control_device_v1_add_listener(self->device, &data_control_device_listener, self);
	self->selection = NULL;
	self->primary_selection = NULL;
	self->cb_data = NULL;
	self->cb_len = 0;
	self->mime_type = "text/plain;charset=utf-8";
}

void data_control_destroy(struct data_control* self)
{
	if (self->selection) {
		zwlr_data_control_source_v1_destroy(self->selection);
		self->selection = NULL;
	}
	if (self->primary_selection) {
		zwlr_data_control_source_v1_destroy(self->primary_selection);
		self->primary_selection = NULL;
	}
	zwlr_data_control_device_v1_destroy(self->device);
	free(self->cb_data);
}

void data_control_to_clipboard(struct data_control* self, const char* text, size_t len)
{
	if (!len) {
		log_error("%s called with 0 length\n", __func__);
		return;
	}
	free(self->cb_data);

	self->cb_data = malloc(len);
	if (!self->cb_data) {
		log_error("OOM: %m\n");
		return;
	}

	memcpy(self->cb_data, text, len);
	self->cb_len = len;
	// Set copy/paste buffer
	self->selection = set_selection(self, false);
	// Set highlight/middle_click buffer
	self->primary_selection = set_selection(self, true);
}
