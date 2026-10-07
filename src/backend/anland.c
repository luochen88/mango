#define _GNU_SOURCE
#ifdef HAVE_ANLAND
#include "mango/backend/anland.h"
#include "mango/common/server.h"
#include "mango/ext-protocol/text-input.h"
#include <anland_audio.h>
#include <anland_de_backend.h>
#include <anland_device.h>
#include <anland_scene.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_touch.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#define RECONNECT_MS 200
#define INPUT_PAYLOAD_MAX ANLAND_DEVICE_MAX_PAYLOAD_SIZE
#define RESOURCE_FD_MAX 64
#define CLIPBOARD_MIME_UTF8 "text/plain;charset=utf-8"
#define CLIPBOARD_MIME_TEXT "text/plain"
#define CLIPBOARD_TRANSFER_TIMEOUT_MS 1000
#define MANGO_WINDOW_ID 1

struct anland_clipboard_read;
struct anland_clipboard_write;
struct anland_buffer {
	struct wlr_buffer base;
	struct wlr_dmabuf_attributes attrs;
};
struct anland_backend;
struct anland_output {
	struct wlr_output base;
	struct anland_backend *backend;
	struct wlr_swapchain *swapchain;
	struct anland_buffer *buffers[ANLAND_DEVICE_MAX_BUFS];
	size_t buffer_count;
	struct wlr_buffer *committed;
	uint64_t committed_buffer_id;
	uint64_t committed_commit_id;
	uint32_t committed_commit_seq;
};

struct anland_backend {
	struct wlr_backend base;
	struct wl_event_loop *loop;
	anland_de_backend *producer;
	anland_device *device;
	anland_scene *scene;
	anland_layer_id layer;
	char *socket_path;
	int drm_fd;
	struct anland_output output;
	struct wlr_keyboard keyboard;
	struct wlr_pointer pointer;
	struct wlr_touch touch;
	struct wl_event_source *data_source, *ready_source, *scene_source, *reconnect_source;
	uint32_t width, height, format, refresh;
	bool announced;
	bool output_initialized;
	bool selection_listener_set;
	struct wl_listener selection_listener;
	bool audio_started;
	struct anland_clipboard_read *clipboard_read;
	struct wl_list clipboard_writes;
	anland_de_target_t target;
	bool have_target;
	uint64_t session_generation;
};

static void cancel_clipboard_read(struct anland_backend *backend);
static void clear_clipboard_writes(struct anland_backend *backend);
static void drop_session(struct anland_backend *backend);
static bool dispatch_scene_events(struct anland_backend *backend);

static const struct wlr_keyboard_impl keyboard_impl = {.name = "anland-keyboard"};
static const struct wlr_pointer_impl pointer_impl = {.name = "anland-pointer"};
static const struct wlr_touch_impl touch_impl = {.name = "anland-touch"};

static uint32_t now_msec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

static double normalized(float value, uint32_t extent) {
	if (extent == 0 || value <= 0) return 0;
	if (value >= extent) return 1;
	return value / extent;
}

static void buffer_destroy(struct wlr_buffer *base) {
	struct anland_buffer *buffer = wl_container_of(base, buffer, base);
	if (buffer->attrs.fd[0] >= 0) close(buffer->attrs.fd[0]);
	free(buffer);
}

static bool buffer_get_dmabuf(struct wlr_buffer *base,
		struct wlr_dmabuf_attributes *attrs) {
	struct anland_buffer *buffer = wl_container_of(base, buffer, base);
	*attrs = buffer->attrs;
	return true;
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_dmabuf = buffer_get_dmabuf,
};

static size_t select_slot(void *data) {
	struct anland_backend *backend = data;
	if (!backend->have_target || backend->target.index >= backend->target.count ||
			backend->target.index >= backend->output.buffer_count) {
		return SIZE_MAX;
	}
	return backend->target.index;
}

static uint32_t protocol_format_to_drm(uint32_t format) {
	switch (format) {
	case 1: /* Android AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM / RGBA_8888 */
		return DRM_FORMAT_ABGR8888;
	default:
		return DRM_FORMAT_INVALID;
	}
}

static void log_dmabuf_info(const anland_device_fb_t *info, uint32_t drm_format) {
	wlr_log(WLR_DEBUG, "Anland dmabuf: protocol format 0x%08"PRIx32" -> DRM %.4s (0x%08"PRIx32"), modifier 0x%016"PRIx64", %ux%u stride %u offset %u",
		info->format, (const char *)&drm_format, drm_format, info->modifier,
		info->width, info->height, info->stride, info->offset);
}

static void clear_target(struct anland_backend *backend) {
	backend->have_target = false;
	backend->session_generation = 0;
	memset(&backend->target, 0, sizeof(backend->target));
}

static void drop_buffers(struct anland_backend *backend) {
	struct anland_output *output = &backend->output;
	if (output->committed) {
		wlr_buffer_unlock(output->committed);
		output->committed = NULL;
	}
	output->committed_buffer_id = 0;
	output->committed_commit_id = 0;
	output->committed_commit_seq = 0;
	wlr_swapchain_destroy(output->swapchain);
	output->swapchain = NULL;
	output->buffer_count = 0;
	memset(output->buffers, 0, sizeof(output->buffers));
	clear_target(backend);
}

static void remove_consumer_sources(struct anland_backend *backend) {
	if (backend->data_source) wl_event_source_remove(backend->data_source);
	if (backend->ready_source) wl_event_source_remove(backend->ready_source);
	if (backend->scene_source) wl_event_source_remove(backend->scene_source);
	backend->data_source = NULL;
	backend->ready_source = NULL;
	backend->scene_source = NULL;
}

static void arm_reconnect(struct anland_backend *backend) {
	if (backend->reconnect_source)
		wl_event_source_timer_update(backend->reconnect_source, RECONNECT_MS);
}

static void disarm_reconnect(struct anland_backend *backend) {
	if (backend->reconnect_source)
		wl_event_source_timer_update(backend->reconnect_source, -1);
}

static void producer_pre_release(void *data) {
	struct anland_backend *backend = data;
	remove_consumer_sources(backend);
	cancel_clipboard_read(backend);
	clear_clipboard_writes(backend);
	anland_audio_set_fd(-1);
	drop_buffers(backend);
	arm_reconnect(backend);
}

static void producer_fallback(void *data) {
	struct anland_backend *backend = data;
	arm_reconnect(backend);
}

static bool import_buffers(struct anland_backend *backend) {
	anland_device_output_t out;
	if (anland_device_get_outputs(backend->device, &out, 1) != 1 ||
			!out.connected || out.width == 0 || out.height == 0) {
		return false;
	}

	int count = anland_device_fb_count(backend->device);
	if (count < 1 || count > ANLAND_DEVICE_MAX_BUFS || count > WLR_SWAPCHAIN_CAP) {
		return false;
	}

	uint32_t drm_format = protocol_format_to_drm(out.format);
	if (drm_format == DRM_FORMAT_INVALID) {
		return false;
	}

	drop_buffers(backend);

	struct wlr_buffer *buffers[ANLAND_DEVICE_MAX_BUFS] = {0};
	struct wlr_drm_format_set formats = {0};
	for (int i = 0; i < count; i++) {
		anland_device_fb_t fb = {.fd = -1};
		if (anland_device_get_fb(backend->device, i, &fb) != 0) {
			goto fail;
		}
		if (fb.width != out.width || fb.height != out.height ||
				fb.format != out.format || fb.stride == 0) {
			close(fb.fd);
			goto fail;
		}
		log_dmabuf_info(&fb, drm_format);
		struct anland_buffer *buffer = calloc(1, sizeof(*buffer));
		if (!buffer) {
			close(fb.fd);
			goto fail;
		}
		buffer->attrs = (struct wlr_dmabuf_attributes){
			.width = fb.width,
			.height = fb.height,
			.format = drm_format,
			.modifier = fb.modifier,
			.n_planes = 1,
			.offset = {fb.offset},
			.stride = {fb.stride},
			.fd = {-1},
		};
		buffer->attrs.fd[0] = fb.fd;
		wlr_buffer_init(&buffer->base, &buffer_impl, fb.width, fb.height);
		backend->output.buffers[i] = buffer;
		buffers[i] = &buffer->base;
		if (!wlr_drm_format_set_add(&formats, drm_format, fb.modifier)) {
			goto fail;
		}
	}

	const struct wlr_drm_format *format = wlr_drm_format_set_get(&formats, drm_format);
	if (!format) {
		goto fail;
	}
	backend->output.swapchain = wlr_swapchain_create_external(out.width, out.height,
		format, buffers, (size_t)count, select_slot, backend);
	if (!backend->output.swapchain) {
		goto fail;
	}
	backend->output.buffer_count = (size_t)count;
	backend->width = out.width;
	backend->height = out.height;
	backend->format = out.format;
	backend->refresh = out.refresh_mhz;
	wlr_drm_format_set_finish(&formats);
	return true;

fail:
	wlr_drm_format_set_finish(&formats);
	for (int i = 0; i < count; i++) {
		if (buffers[i]) wlr_buffer_drop(buffers[i]);
	}
	backend->output.buffer_count = 0;
	memset(backend->output.buffers, 0, sizeof(backend->output.buffers));
	return false;
}

static bool apply_output_change(struct anland_backend *backend) {
	anland_device_output_t out;
	if (anland_de_backend_get_output(backend->producer, &out) != 0 ||
			!out.connected || out.width == 0 || out.height == 0 ||
			protocol_format_to_drm(out.format) == DRM_FORMAT_INVALID) {
		return false;
	}
	backend->width = out.width;
	backend->height = out.height;
	backend->format = out.format;
	backend->refresh = out.refresh_mhz;
	if (backend->output_initialized) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_custom_mode(&state, backend->width,
			backend->height, backend->refresh);
		wlr_output_send_request_state(&backend->output.base, &state);
		wlr_output_state_finish(&state);
	}
	return true;
}

static void announce_devices(struct anland_backend *backend) {
	if (backend->announced) return;
	backend->announced = true;
	wl_signal_emit_mutable(&backend->base.events.new_output, &backend->output.base);
	wl_signal_emit_mutable(&backend->base.events.new_input, &backend->keyboard.base);
	wl_signal_emit_mutable(&backend->base.events.new_input, &backend->pointer.base);
	wl_signal_emit_mutable(&backend->base.events.new_input, &backend->touch.base);
}

static bool refresh_target(struct anland_backend *backend,
		const anland_scene_event_t *event) {
	anland_de_target_t target;
	if (anland_de_backend_get_target(backend->producer, &target) != 0) {
		return false;
	}
	if (event && (event->u.target_ready.generation != target.generation ||
				event->u.target_ready.index != target.index ||
				event->u.target_ready.count != target.count)) {
		return false;
	}
	if (target.generation == 0 || target.count == 0 ||
			target.index >= target.count || target.count > ANLAND_DEVICE_MAX_BUFS ||
			target.count > WLR_SWAPCHAIN_CAP || target.width == 0 ||
			target.height == 0 || protocol_format_to_drm(target.format) == DRM_FORMAT_INVALID) {
		return false;
	}
	if (backend->session_generation != 0 &&
			backend->session_generation != target.generation) {
		return false;
	}
	if (!backend->output.swapchain || backend->output.buffer_count != target.count ||
			backend->width != target.width || backend->height != target.height ||
			backend->format != target.format) {
		backend->target = target;
		backend->have_target = true;
		backend->session_generation = target.generation;
		if (!import_buffers(backend)) {
			clear_target(backend);
			return false;
		}
	}
	backend->target = target;
	backend->have_target = true;
	backend->session_generation = target.generation;
	return true;
}

static void send_present_event(struct anland_backend *backend) {
	struct wlr_output_event_present event = {
		.commit_seq = backend->output.committed_commit_seq,
		.presented = true,
	};
	wlr_output_send_present(&backend->output.base, &event);
}

static bool handle_scene_event(struct anland_backend *backend,
		const anland_scene_event_t *event) {
	switch (event->type) {
	case ANLAND_SCENE_EVENT_PRESENTED:
		if (backend->output.committed_commit_id == 0 ||
				backend->output.committed_commit_id != event->commit_id) {
			return false;
		}
		backend->output.committed_commit_id = 0;
		send_present_event(backend);
		return true;
	case ANLAND_SCENE_EVENT_BUFFER_RELEASED:
		if (event->u.released.release_fence_fd >= 0) {
			close(event->u.released.release_fence_fd);
		}
		if (backend->output.committed &&
				backend->output.committed_buffer_id == event->u.released.buffer_id) {
			wlr_buffer_unlock(backend->output.committed);
			backend->output.committed = NULL;
			backend->output.committed_buffer_id = 0;
			backend->output.committed_commit_seq = 0;
			return true;
		}
		return backend->output.committed_buffer_id == 0;
	case ANLAND_SCENE_EVENT_COMMIT_DROPPED:
		if (backend->output.committed_commit_id == event->commit_id) {
			backend->output.committed_commit_id = 0;
			return true;
		}
		return backend->output.committed_commit_id == 0 || event->commit_id == 0;
	case ANLAND_SCENE_EVENT_OUTPUT_CHANGED:
		if (event->u.output.width == 0 || event->u.output.height == 0 ||
				!apply_output_change(backend)) {
			return false;
		}
		announce_devices(backend);
		return true;
	case ANLAND_SCENE_EVENT_RENDER_TARGET_READY:
		if (event->u.target_ready.count == 0 ||
				event->u.target_ready.index >= event->u.target_ready.count ||
				event->u.target_ready.count > ANLAND_DEVICE_MAX_BUFS ||
				event->u.target_ready.count > WLR_SWAPCHAIN_CAP ||
				!refresh_target(backend, event)) {
			return false;
		}
		if (!backend->announced && apply_output_change(backend)) {
			announce_devices(backend);
		}
		wlr_output_update_needs_frame(&backend->output.base);
		wlr_output_send_frame(&backend->output.base);
		return true;
	}
	return true;
}

static bool dispatch_scene_events(struct anland_backend *backend) {
	for (;;) {
		anland_scene_event_t events[ANLAND_SCENE_EVENT_QUEUE];
		size_t count = 0;
		if (anland_de_backend_dispatch(backend->producer, events,
					ANLAND_SCENE_EVENT_QUEUE, &count) != 0) {
			return false;
		}
		if (count == 0) {
			return true;
		}
		for (size_t i = 0; i < count; i++) {
			if (!handle_scene_event(backend, &events[i])) {
				return false;
			}
		}
		if (count < ANLAND_SCENE_EVENT_QUEUE) {
			return true;
		}
	}
}

static void drop_session(struct anland_backend *backend) {
	producer_pre_release(backend);
	anland_de_backend_drop_session(backend->producer);
	(void)dispatch_scene_events(backend);
	arm_reconnect(backend);
}

static int handle_ready(int fd, uint32_t mask, void *data) {
	(void)fd;
	struct anland_backend *backend = data;
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		drop_session(backend);
		return 0;
	}
	if (anland_de_backend_pump(backend->producer, 0) != 0 ||
			!dispatch_scene_events(backend)) {
		drop_session(backend);
	}
	return 0;
}

static int handle_scene(int fd, uint32_t mask, void *data) {
	(void)fd;
	struct anland_backend *backend = data;
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		drop_session(backend);
		return 0;
	}
	if (!dispatch_scene_events(backend)) {
		drop_session(backend);
	}
	return 0;
}

static void emit_pointer_frame(struct anland_backend *backend) {
	wl_signal_emit_mutable(&backend->pointer.events.frame, &backend->pointer);
}

struct anland_clipboard_source {
	struct wlr_data_source source;
	struct anland_backend *backend;
	char *data;
	size_t size;
};

struct anland_clipboard_write {
	struct anland_backend *backend;
	struct wl_list link;
	struct wl_event_source *source;
	int fd;
	char *data;
	size_t size, offset;
};

static void clipboard_write_destroy(struct anland_clipboard_write *transfer) {
	if (transfer->source) wl_event_source_remove(transfer->source);
	if (transfer->fd >= 0) close(transfer->fd);
	wl_list_remove(&transfer->link);
	free(transfer->data);
	free(transfer);
}

static void clear_clipboard_writes(struct anland_backend *backend) {
	struct anland_clipboard_write *transfer, *tmp;
	wl_list_for_each_safe(transfer, tmp, &backend->clipboard_writes, link)
		clipboard_write_destroy(transfer);
}

static int handle_clipboard_write(int fd, uint32_t mask, void *data) {
	struct anland_clipboard_write *transfer = data;
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		clipboard_write_destroy(transfer);
		return 0;
	}
	while (transfer->offset < transfer->size) {
		ssize_t n = write(fd, transfer->data + transfer->offset,
			transfer->size - transfer->offset);
		if (n > 0) { transfer->offset += (size_t)n; continue; }
		if (n < 0 && errno == EINTR) continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
		break;
	}
	clipboard_write_destroy(transfer);
	return 0;
}

static void clipboard_source_send(struct wlr_data_source *source,
		const char *mime_type, int32_t fd) {
	struct anland_clipboard_source *clip = wl_container_of(source, clip, source);
	if (strcmp(mime_type, CLIPBOARD_MIME_UTF8) != 0 &&
			strcmp(mime_type, CLIPBOARD_MIME_TEXT) != 0) {
		close(fd);
		return;
	}
	struct anland_backend *backend = clip->backend;
	struct anland_clipboard_write *transfer = calloc(1, sizeof(*transfer));
	if (!transfer) { close(fd); return; }
	transfer->data = malloc(clip->size);
	if (!transfer->data) { close(fd); free(transfer); return; }
	memcpy(transfer->data, clip->data, clip->size);
	transfer->backend = backend;
	transfer->fd = fd;
	transfer->size = clip->size;
	if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) {
		close(fd); free(transfer->data); free(transfer); return;
	}
	wl_list_insert(&backend->clipboard_writes, &transfer->link);
	transfer->source = wl_event_loop_add_fd(backend->loop, fd, WL_EVENT_WRITABLE,
		handle_clipboard_write, transfer);
	if (!transfer->source) clipboard_write_destroy(transfer);
}

static void clipboard_source_destroy(struct wlr_data_source *source) {
	struct anland_clipboard_source *clip = wl_container_of(source, clip, source);
	free(clip->data);
	free(clip);
}

static const struct wlr_data_source_impl clipboard_source_impl = {
	.send = clipboard_source_send,
	.destroy = clipboard_source_destroy,
};

static bool clipboard_source_add_mime(struct wlr_data_source *source,
		const char *mime) {
	char *copy = strdup(mime);
	if (!copy)
		return false;
	char **slot = wl_array_add(&source->mime_types, sizeof(*slot));
	if (!slot) {
		free(copy);
		return false;
	}
	*slot = copy;
	return true;
}

static void set_clipboard_from_consumer(struct anland_backend *backend,
		const char *payload, size_t size) {
	if (!server.seat || size == 0)
		return;
	struct anland_clipboard_source *clip = calloc(1, sizeof(*clip));
	if (!clip)
		return;
	clip->backend = backend;
	clip->data = malloc(size);
	if (!clip->data) {
		free(clip);
		return;
	}
	memcpy(clip->data, payload, size);
	clip->size = size;
	wlr_data_source_init(&clip->source, &clipboard_source_impl);
	if (!clipboard_source_add_mime(&clip->source, CLIPBOARD_MIME_UTF8) ||
			!clipboard_source_add_mime(&clip->source, CLIPBOARD_MIME_TEXT)) {
		wlr_data_source_destroy(&clip->source);
		return;
	}
	wlr_seat_set_selection(server.seat, &clip->source, now_msec());
}

static const char *pick_text_mime(struct wlr_data_source *source) {
	const char *fallback = NULL;
	char **mime;
	wl_array_for_each(mime, &source->mime_types) {
		if (strcmp(*mime, CLIPBOARD_MIME_UTF8) == 0)
			return CLIPBOARD_MIME_UTF8;
		if (strcmp(*mime, CLIPBOARD_MIME_TEXT) == 0)
			fallback = CLIPBOARD_MIME_TEXT;
	}
	return fallback;
}

struct anland_clipboard_read {
	struct anland_backend *backend;
	struct wl_event_source *source, *timer;
	int fd;
	char *data;
	size_t size, capacity;
};

static void clipboard_read_destroy(struct anland_clipboard_read *transfer) {
	if (transfer->source) wl_event_source_remove(transfer->source);
	if (transfer->timer) wl_event_source_remove(transfer->timer);
	if (transfer->fd >= 0) close(transfer->fd);
	transfer->backend->clipboard_read = NULL;
	free(transfer->data);
	free(transfer);
}

static void finish_clipboard_read(struct anland_clipboard_read *transfer) {
	if (transfer->size > 0 &&
			anland_device_is_connected(transfer->backend->device)) {
		(void)anland_device_set_clipboard(transfer->backend->device,
			transfer->data, transfer->size);
	}
	clipboard_read_destroy(transfer);
}

static int handle_clipboard_timeout(void *data) {
	clipboard_read_destroy(data);
	return 0;
}

static int handle_clipboard_read(int fd, uint32_t mask, void *data) {
	struct anland_clipboard_read *transfer = data;
	if (mask & WL_EVENT_ERROR) {
		clipboard_read_destroy(transfer);
		return 0;
	}
	for (;;) {
		if (transfer->size == transfer->capacity) {
			size_t next = transfer->capacity ? transfer->capacity * 2 : 4096;
			if (next > INPUT_PAYLOAD_MAX) next = INPUT_PAYLOAD_MAX;
			if (next == transfer->capacity) { finish_clipboard_read(transfer); return 0; }
			char *tmp = realloc(transfer->data, next);
			if (!tmp) { clipboard_read_destroy(transfer); return 0; }
			transfer->data = tmp;
			transfer->capacity = next;
		}
		ssize_t n = read(fd, transfer->data + transfer->size,
			transfer->capacity - transfer->size);
		if (n > 0) { transfer->size += (size_t)n; continue; }
		if (n == 0) { finish_clipboard_read(transfer); return 0; }
		if (errno == EINTR) continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) break;
		clipboard_read_destroy(transfer);
		return 0;
	}
	if (mask & WL_EVENT_HANGUP) finish_clipboard_read(transfer);
	return 0;
}

static void cancel_clipboard_read(struct anland_backend *backend) {
	if (backend->clipboard_read)
		clipboard_read_destroy(backend->clipboard_read);
}

static void send_selection_to_consumer(struct anland_backend *backend,
		struct wlr_data_source *source) {
	cancel_clipboard_read(backend);
	if (!source || source->impl == &clipboard_source_impl ||
			!anland_device_is_connected(backend->device)) return;
	const char *mime = pick_text_mime(source);
	if (!mime) return;
	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0) return;
	struct anland_clipboard_read *transfer = calloc(1, sizeof(*transfer));
	if (!transfer) { close(pipefd[0]); close(pipefd[1]); return; }
	transfer->backend = backend;
	transfer->fd = pipefd[0];
	backend->clipboard_read = transfer;
	transfer->source = wl_event_loop_add_fd(backend->loop, pipefd[0],
		WL_EVENT_READABLE, handle_clipboard_read, transfer);
	transfer->timer = wl_event_loop_add_timer(backend->loop,
		handle_clipboard_timeout, transfer);
	if (!transfer->source || !transfer->timer) {
		if (transfer->source) wl_event_source_remove(transfer->source);
		if (transfer->timer) wl_event_source_remove(transfer->timer);
		backend->clipboard_read = NULL;
		close(pipefd[0]); close(pipefd[1]); free(transfer); return;
	}
	wl_event_source_timer_update(transfer->timer, CLIPBOARD_TRANSFER_TIMEOUT_MS);
	wlr_data_source_send(source, mime, pipefd[1]);
	wl_display_flush_clients(server.display);
}

static void handle_seat_set_selection(struct wl_listener *listener, void *data) {
	(void)data;
	struct anland_backend *backend = wl_container_of(listener, backend,
		selection_listener);
	send_selection_to_consumer(backend,
		server.seat ? server.seat->selection_source : NULL);
}

static bool read_text_payload(struct anland_backend *backend, uint32_t type,
		size_t size) {
	if (size == 0 || size > ANLAND_DEVICE_MAX_PAYLOAD_SIZE) {
		return false;
	}
	char *payload = malloc(size + 1);
	if (!payload) {
		return false;
	}
	int rc = anland_device_read_input(backend->device, payload, size,
		CLIPBOARD_TRANSFER_TIMEOUT_MS);
	if (rc != 1) {
		free(payload);
		return false;
	}
	payload[size] = '\0';
	if (type == ANLAND_DEVICE_IN_TEXT_INPUT) {
		mango_text_input_commit_utf8(server.input_method_relay, payload, size);
	} else {
		set_clipboard_from_consumer(backend, payload, size);
	}
	free(payload);
	return true;
}

static bool consume_resource_fds(struct anland_backend *backend, uint32_t fdnum) {
	if (fdnum == 0) {
		return true;
	}
	if (fdnum > RESOURCE_FD_MAX) {
		return false;
	}
	int fds[RESOURCE_FD_MAX];
	int count = 0;
	for (size_t i = 0; i < RESOURCE_FD_MAX; i++) fds[i] = -1;
	int rc = anland_device_read_fds(backend->device, fds, RESOURCE_FD_MAX,
		&count, CLIPBOARD_TRANSFER_TIMEOUT_MS);
	for (int i = 0; i < count; i++) {
		if (fds[i] >= 0) close(fds[i]);
	}
	return rc == 1 && count == (int)fdnum;
}

static int handle_input(int fd, uint32_t mask, void *data) {
	(void)fd;
	struct anland_backend *backend = data;
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		drop_session(backend);
		return 0;
	}
	anland_device_input_t ev;
	int ret;
	while ((ret = anland_device_poll_input(backend->device, &ev, 0)) > 0) {
		uint32_t time = now_msec();
		switch (ev.type) {
		case ANLAND_DEVICE_IN_KEY: {
			struct wlr_keyboard_key_event e = {
				.time_msec = time,
				.keycode = ev.key.keycode,
				.update_state = true,
				.state = ev.key.action == ANLAND_DEVICE_ACTION_DOWN ?
					WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
			};
			wlr_keyboard_notify_key(&backend->keyboard, &e);
			break;
		}
		case ANLAND_DEVICE_IN_PTR_MOTION: {
			struct wlr_pointer_motion_absolute_event a = {
				.pointer = &backend->pointer,
				.time_msec = time,
				.x = normalized(ev.pointer_motion.x, backend->width),
				.y = normalized(ev.pointer_motion.y, backend->height),
			};
			wl_signal_emit_mutable(&backend->pointer.events.motion_absolute, &a);
			struct wlr_pointer_motion_event r = {
				.pointer = &backend->pointer,
				.time_msec = time,
				.delta_x = ev.pointer_motion.dx,
				.delta_y = ev.pointer_motion.dy,
				.unaccel_dx = ev.pointer_motion.dx,
				.unaccel_dy = ev.pointer_motion.dy,
			};
			wl_signal_emit_mutable(&backend->pointer.events.motion, &r);
			emit_pointer_frame(backend);
			break;
		}
		case ANLAND_DEVICE_IN_PTR_BUTTON: {
			struct wlr_pointer_button_event e = {
				.pointer = &backend->pointer,
				.time_msec = time,
				.button = ev.pointer_button.button,
				.state = ev.pointer_button.pressed ?
					WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED,
			};
			wlr_pointer_notify_button(&backend->pointer, &e);
			emit_pointer_frame(backend);
			break;
		}
		case ANLAND_DEVICE_IN_PTR_AXIS: {
			struct wlr_pointer_axis_event e = {
				.pointer = &backend->pointer,
				.time_msec = time,
				.source = WL_POINTER_AXIS_SOURCE_WHEEL,
				.orientation = ev.pointer_axis.axis == 0 ?
					WL_POINTER_AXIS_VERTICAL_SCROLL : WL_POINTER_AXIS_HORIZONTAL_SCROLL,
				.relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
				.delta = ev.pointer_axis.value,
				.delta_discrete = ev.pointer_axis.discrete * WLR_POINTER_AXIS_DISCRETE_STEP,
			};
			wl_signal_emit_mutable(&backend->pointer.events.axis, &e);
			emit_pointer_frame(backend);
			break;
		}
		case ANLAND_DEVICE_IN_TOUCH: {
			double x = normalized(ev.touch.x, backend->width);
			double y = normalized(ev.touch.y, backend->height);
			if (ev.touch.action == ANLAND_DEVICE_ACTION_DOWN) {
				struct wlr_touch_down_event e = {.touch = &backend->touch,
					.time_msec = time, .touch_id = ev.touch.pointer_id,
					.x = x, .y = y};
				wl_signal_emit_mutable(&backend->touch.events.down, &e);
			} else if (ev.touch.action == ANLAND_DEVICE_ACTION_MOVE) {
				struct wlr_touch_motion_event e = {.touch = &backend->touch,
					.time_msec = time, .touch_id = ev.touch.pointer_id,
					.x = x, .y = y};
				wl_signal_emit_mutable(&backend->touch.events.motion, &e);
			} else {
				struct wlr_touch_up_event e = {.touch = &backend->touch,
					.time_msec = time, .touch_id = ev.touch.pointer_id};
				wl_signal_emit_mutable(&backend->touch.events.up, &e);
			}
			break;
		}
		case ANLAND_DEVICE_IN_TOUCH_FRAME:
			wl_signal_emit_mutable(&backend->touch.events.frame, &backend->touch);
			break;
		case ANLAND_DEVICE_IN_DISPLAY_REFRESH:
			if (ev.display.refresh_mhz && ev.display.refresh_mhz != backend->refresh) {
				backend->refresh = ev.display.refresh_mhz;
				struct wlr_output_state state;
				wlr_output_state_init(&state);
				wlr_output_state_set_custom_mode(&state, backend->width,
					backend->height, backend->refresh);
				wlr_output_send_request_state(&backend->output.base, &state);
				wlr_output_state_finish(&state);
			}
			break;
		case ANLAND_DEVICE_IN_CLIPBOARD:
			if (!read_text_payload(backend, ev.type, ev.clipboard.size)) {
				drop_session(backend);
				return 0;
			}
			break;
		case ANLAND_DEVICE_IN_TEXT_INPUT:
			if (!read_text_payload(backend, ev.type, ev.text_input.size)) {
				drop_session(backend);
				return 0;
			}
			break;
		case ANLAND_DEVICE_IN_RESOURCE:
			if (!consume_resource_fds(backend, ev.resource.fdnum)) {
				drop_session(backend);
				return 0;
			}
			break;
		case ANLAND_DEVICE_IN_RESOURCE_INVALID:
			break;
		default:
			break;
		}
	}
	if (ret < 0) {
		drop_session(backend);
	}
	return 0;
}

static bool attach_sources(struct anland_backend *backend) {
	remove_consumer_sources(backend);
	int data_fd = anland_device_data_fd(backend->device);
	int ready_fd = anland_device_buffer_ready_fd(backend->device);
	int scene_fd = anland_scene_event_fd(backend->scene);
	if (data_fd < 0 || ready_fd < 0 || scene_fd < 0) {
		return false;
	}
	int flags = fcntl(data_fd, F_GETFL);
	if (flags < 0 || fcntl(data_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		return false;
	}
	backend->data_source = wl_event_loop_add_fd(backend->loop, data_fd,
		WL_EVENT_READABLE, handle_input, backend);
	backend->ready_source = wl_event_loop_add_fd(backend->loop, ready_fd,
		WL_EVENT_READABLE, handle_ready, backend);
	backend->scene_source = wl_event_loop_add_fd(backend->loop, scene_fd,
		WL_EVENT_READABLE, handle_scene, backend);
	return backend->data_source && backend->ready_source && backend->scene_source;
}

static int reconnect(void *data) {
	struct anland_backend *backend = data;
	(void)dispatch_scene_events(backend);
	if (!anland_device_is_daemon_alive(backend->device)) {
		producer_pre_release(backend);
		anland_de_backend_drop_session(backend->producer);
		(void)dispatch_scene_events(backend);
		if (anland_de_backend_reopen(backend->producer, backend->socket_path) != 0) {
			arm_reconnect(backend);
			return 0;
		}
	}
	if (anland_de_backend_reconnect(backend->producer) != 0) {
		arm_reconnect(backend);
		return 0;
	}
	if (!import_buffers(backend) || !attach_sources(backend)) {
		drop_session(backend);
		return 0;
	}
	anland_audio_set_fd(anland_device_audio_fd(backend->device));
	if (!dispatch_scene_events(backend)) {
		drop_session(backend);
		return 0;
	}
	disarm_reconnect(backend);
	return 0;
}

static bool export_write_fence(struct wlr_buffer *buffer, int *out_fd) {
	*out_fd = -1;
	struct wlr_dmabuf_attributes attrs;
	if (!wlr_buffer_get_dmabuf(buffer, &attrs) || attrs.n_planes < 1 || attrs.fd[0] < 0) {
		return true;
	}
	struct dma_buf_export_sync_file sync = {.flags = DMA_BUF_SYNC_WRITE, .fd = -1};
	if (ioctl(attrs.fd[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &sync) == 0) {
		*out_fd = sync.fd;
	}
	return true;
}

static bool present_buffer(struct anland_output *output, struct wlr_buffer *buffer) {
	struct anland_backend *backend = output->backend;
	if (!backend->have_target || backend->target.index >= output->buffer_count ||
			!output->buffers[backend->target.index] ||
			buffer != &output->buffers[backend->target.index]->base ||
			output->committed) {
		return false;
	}
	int fence_fd = -1;
	if (!export_write_fence(buffer, &fence_fd)) {
		return false;
	}
	anland_layer_state_t state = {
		.layer_id = backend->layer,
		.buffer_id = (uint64_t)backend->target.index + 1,
		.destination = {
			.x = 0,
			.y = 0,
			.width = backend->width,
			.height = backend->height,
		},
		.opacity = 1.0f,
		.visible = true,
		.acquire_fence_fd = fence_fd,
	};
	uint64_t commit_id = 0;
	if (anland_de_backend_commit(backend->producer, &state, 1, &commit_id) != 0) {
		if (fence_fd >= 0) close(fence_fd);
		return false;
	}
	if (fence_fd >= 0) close(fence_fd);
	if (anland_de_backend_present(backend->producer) != 0) {
		drop_session(backend);
		return false;
	}
	output->committed = wlr_buffer_lock(buffer);
	output->committed_buffer_id = state.buffer_id;
	output->committed_commit_id = commit_id;
	output->committed_commit_seq = output->base.commit_seq + 1;
	return true;
}

static const uint32_t output_states = WLR_OUTPUT_STATE_BACKEND_OPTIONAL |
	WLR_OUTPUT_STATE_BUFFER | WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE;

static bool output_test(struct wlr_output *base,
		const struct wlr_output_state *state) {
	struct anland_output *output = wl_container_of(base, output, base);
	if (state->committed & ~output_states) {
		return false;
	}
	if ((state->committed & WLR_OUTPUT_STATE_MODE) &&
			(state->mode_type != WLR_OUTPUT_STATE_MODE_CUSTOM ||
			state->custom_mode.width != (int32_t)output->backend->width ||
			state->custom_mode.height != (int32_t)output->backend->height ||
			state->custom_mode.refresh != (int32_t)output->backend->refresh)) {
		return false;
	}
	if ((state->committed & WLR_OUTPUT_STATE_BUFFER) &&
			(!output->swapchain || !state->buffer || output->committed ||
			!output->backend->have_target ||
			!wlr_swapchain_has_buffer(output->swapchain, state->buffer))) {
		return false;
	}
	return true;
}

static bool output_commit(struct wlr_output *base,
		const struct wlr_output_state *state) {
	struct anland_output *output = wl_container_of(base, output, base);
	if (!output_test(base, state)) return false;
	if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
		if (!present_buffer(output, state->buffer))
			return false;
	}
	return true;
}

static void output_destroy(struct wlr_output *base) {
	struct anland_output *output = wl_container_of(base, output, base);
	drop_buffers(output->backend);
	wlr_output_finish(base);
}

static const struct wlr_output_impl output_impl = {
	.destroy = output_destroy,
	.test = output_test,
	.commit = output_commit,
};

bool mango_anland_output_is(struct wlr_output *output) {
	return output && output->impl == &output_impl;
}

struct wlr_swapchain *mango_anland_output_swapchain(struct wlr_output *output) {
	if (!mango_anland_output_is(output)) return NULL;
	struct anland_output *anland = wl_container_of(output, anland, base);
	return anland->swapchain;
}

bool mango_anland_touch_is(struct wlr_touch *touch) {
	return touch && touch->impl == &touch_impl;
}

static bool backend_start(struct wlr_backend *base) {
	struct anland_backend *backend = wl_container_of(base, backend, base);
	if (anland_audio_start() == 0)
		backend->audio_started = true;
	if (server.seat && !backend->selection_listener_set) {
		backend->selection_listener.notify = handle_seat_set_selection;
		wl_signal_add(&server.seat->events.set_selection, &backend->selection_listener);
		backend->selection_listener_set = true;
	}
	reconnect(backend);
	return true;
}

static void backend_destroy(struct wlr_backend *base) {
	struct anland_backend *backend = wl_container_of(base, backend, base);
	if (backend->selection_listener_set) wl_list_remove(&backend->selection_listener.link);
	if (backend->reconnect_source) wl_event_source_remove(backend->reconnect_source);
	backend->reconnect_source = NULL;
	producer_pre_release(backend);
	if (backend->audio_started) anland_audio_stop();
	if (backend->output_initialized) wlr_output_destroy(&backend->output.base);
	wlr_keyboard_finish(&backend->keyboard);
	wlr_pointer_finish(&backend->pointer);
	wlr_touch_finish(&backend->touch);
	wlr_backend_finish(base);
	anland_de_backend_destroy(backend->producer);
	if (backend->drm_fd >= 0) close(backend->drm_fd);
	free(backend->socket_path);
	free(backend);
}

static int backend_get_drm_fd(struct wlr_backend *base) {
	struct anland_backend *backend = wl_container_of(base, backend, base);
	return backend->drm_fd;
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_drm_fd = backend_get_drm_fd,
};

struct wlr_backend *mango_anland_backend_create(struct wl_event_loop *loop,
		const char *socket_path) {
	struct anland_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) return NULL;
	backend->socket_path = strdup(socket_path ? socket_path : "");
	if (!backend->socket_path) { free(backend); return NULL; }
	backend->loop = loop;
	backend->drm_fd = -1;
	wl_list_init(&backend->clipboard_writes);

	const char *drm_path = getenv("ANLAND_DRM_DEVICE");
	if (!drm_path || !*drm_path) drm_path = getenv("WLR_RENDER_DRM_DEVICE");
	if (drm_path && *drm_path) {
		backend->drm_fd = open(drm_path, O_RDWR | O_CLOEXEC);
		if (backend->drm_fd < 0) goto fail;
	}

	anland_present_config_t present = {.endpoint = socket_path};
	anland_de_backend_config_t config = {.present = present, .name = "mango"};
	backend->producer = anland_de_backend_create(&config);
	if (!backend->producer) goto fail;
	backend->device = anland_de_backend_device(backend->producer);
	backend->scene = anland_de_backend_scene(backend->producer);
	if (!backend->device || !backend->scene) goto fail;

	anland_device_output_t out;
	if (anland_device_get_outputs(backend->device, &out, 1) != 1 ||
			out.width == 0 || out.height == 0 ||
			protocol_format_to_drm(out.format) == DRM_FORMAT_INVALID) {
		goto fail;
	}
	backend->width = out.width;
	backend->height = out.height;
	backend->format = out.format;
	backend->refresh = out.refresh_mhz;

	anland_layer_desc_t layer = {
		.kind = ANLAND_LAYER_NORMAL,
		.name = "mango composite",
		.geometry = {.x = 0, .y = 0, .width = backend->width, .height = backend->height},
		.opacity = 1.0f,
		.visible = true,
	};
	if (anland_de_backend_add_window_desc(backend->producer, MANGO_WINDOW_ID,
			&layer, &backend->layer) != 0) {
		goto fail;
	}

	wlr_backend_init(&backend->base, &backend_impl);
	backend->base.buffer_caps = WLR_BUFFER_CAP_DMABUF;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, backend->width, backend->height,
		backend->refresh);
	backend->output.backend = backend;
	wlr_output_init(&backend->output.base, &backend->base, &output_impl, loop, &state);
	backend->output_initialized = true;
	wlr_output_state_finish(&state);
	wlr_output_set_name(&backend->output.base, "ANLAND-1");
	wlr_output_set_description(&backend->output.base, "Anland 5 display");
	wlr_keyboard_init(&backend->keyboard, &keyboard_impl, "Anland keyboard");
	wlr_pointer_init(&backend->pointer, &pointer_impl, "Anland pointer");
	wlr_touch_init(&backend->touch, &touch_impl, "Anland touch");
	backend->reconnect_source = wl_event_loop_add_timer(loop, reconnect, backend);
	if (!backend->reconnect_source) goto fail_initialized;
	anland_device_set_pre_release_cb(backend->device, producer_pre_release, backend);
	anland_device_set_fallback_cb(backend->device, producer_fallback, backend);
	return &backend->base;

fail_initialized:
	wlr_output_destroy(&backend->output.base);
	wlr_keyboard_finish(&backend->keyboard);
	wlr_pointer_finish(&backend->pointer);
	wlr_touch_finish(&backend->touch);
	wlr_backend_finish(&backend->base);
fail:
	if (backend->producer) anland_de_backend_destroy(backend->producer);
	if (backend->drm_fd >= 0) close(backend->drm_fd);
	free(backend->socket_path);
	free(backend);
	return NULL;
}
#endif
