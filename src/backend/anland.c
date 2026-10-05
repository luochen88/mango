#define _GNU_SOURCE
#ifdef HAVE_ANLAND
#include "mango/backend/anland.h"
#include "mango/common/server.h"
#include "mango/ext-protocol/text-input.h"
#include <display_producer.h>
#include <anland_audio.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
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
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/util/log.h>

#define RECONNECT_MS 200
#define INPUT_PAYLOAD_MAX (4U * 1024U * 1024U)
#define CLIPBOARD_MIME_UTF8 "text/plain;charset=utf-8"
#define CLIPBOARD_MIME_TEXT "text/plain"
#define CLIPBOARD_TRANSFER_TIMEOUT_MS 1000

struct anland_clipboard_read;
struct anland_clipboard_write;
struct anland_output_message;
struct anland_buffer {
	struct wlr_buffer base;
	struct wlr_dmabuf_attributes attrs;
};
struct anland_backend;
struct anland_output {
	struct wlr_output base;
	struct anland_backend *backend;
	struct wlr_swapchain *swapchain;
	struct anland_buffer *buffers[MAX_BUFS];
	struct wlr_buffer *inflight;   /* presented to the consumer, awaiting ready */
	struct wlr_buffer *queued;     /* rendered while inflight, awaiting a ready */
};

static bool present_buffer(struct anland_output *output, struct wlr_buffer *buffer);
struct anland_backend {
	struct wlr_backend base;
	struct wl_event_loop *loop;
	display_ctx *display;
	char *socket_path;
	int drm_fd;
	struct anland_output output;
	struct wlr_keyboard keyboard;
	struct wlr_pointer pointer;
	struct wlr_touch touch;
	struct wl_event_source *data_source, *ready_source, *reconnect_source;
	uint32_t width, height, format, refresh;
	bool announced;
	bool output_initialized;
	bool selection_listener_set;
	struct wl_listener selection_listener;
	bool audio_started;
	struct anland_clipboard_read *clipboard_read;
	struct wl_list clipboard_writes, output_messages;
	struct wl_event_source *output_timer;
	uint8_t input_header[sizeof(struct data_msg) + sizeof(struct InputEvent)];
	size_t input_header_offset;
	char *input_payload;
	size_t input_payload_size, input_payload_offset;
	uint32_t input_payload_type;
 };

static void cancel_clipboard_read(struct anland_backend *backend);
static void clear_clipboard_writes(struct anland_backend *backend);
static void clear_input_payload(struct anland_backend *backend);
static void clear_output_messages(struct anland_backend *backend);

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
	int index = get_selected_idx(backend->display);
	return index < 0 ? SIZE_MAX : (size_t)index;
}
static void drop_buffers(struct anland_backend *backend) {
	struct anland_output *output = &backend->output;
	if (output->inflight) {
		wlr_buffer_unlock(output->inflight);
		output->inflight = NULL;
	}
	if (output->queued) {
		wlr_buffer_unlock(output->queued);
		output->queued = NULL;
	}
	wlr_swapchain_destroy(output->swapchain);
	output->swapchain = NULL;
	memset(output->buffers, 0, sizeof(output->buffers));
}
static void remove_consumer_sources(struct anland_backend *backend) {
	if (backend->data_source) wl_event_source_remove(backend->data_source);
	if (backend->ready_source) wl_event_source_remove(backend->ready_source);
	backend->data_source = backend->ready_source = NULL;
}
static void producer_pre_release(void *data) {
	struct anland_backend *backend = data;
	remove_consumer_sources(backend);
	cancel_clipboard_read(backend);
	clear_clipboard_writes(backend);
	clear_output_messages(backend);
	clear_input_payload(backend);
	anland_audio_set_fd(-1);
	drop_buffers(backend);
}
static void producer_fallback(void *data) {
	struct anland_backend *backend = data;
	if (backend->reconnect_source)
		wl_event_source_timer_update(backend->reconnect_source, RECONNECT_MS);
}
static uint32_t protocol_format_to_drm(uint32_t format) {
	switch (format) {
	case 1: /* Android AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM / RGBA_8888 */
		return DRM_FORMAT_ABGR8888;
	default:
		return DRM_FORMAT_XRGB8888;
	}
}

static void log_dmabuf_info(const struct buf_info *info, uint32_t drm_format) {
	wlr_log(WLR_DEBUG, "Anland dmabuf: protocol format 0x%08"PRIx32" -> DRM %.4s (0x%08"PRIx32"), modifier 0x%016"PRIx64", %ux%u stride %u offset %u",
		info->format, (const char *)&drm_format, drm_format, info->modifier,
		info->width, info->height, info->stride, info->offset);
}
static bool import_buffers(struct anland_backend *backend) {
	int count = get_buf_count(backend->display);
	if (count < 1 || count > MAX_BUFS || count > WLR_SWAPCHAIN_CAP) return false;
	struct wlr_buffer *buffers[MAX_BUFS] = {0};
	struct wlr_drm_format_set formats = {0};
	for (int i = 0; i < count; i++) {
		struct buf_info info;
		int source_fd = get_dmabuf_fd_at(backend->display, i);
		if (source_fd < 0 || get_dmabuf_info_at(backend->display, i, &info) < 0 ||
			info.width != backend->width || info.height != backend->height ||
			info.format != backend->format || info.stride == 0) goto fail;
		uint32_t drm_format = protocol_format_to_drm(info.format);
		log_dmabuf_info(&info, drm_format);
		struct anland_buffer *buffer = calloc(1, sizeof(*buffer));
		if (!buffer) goto fail;
		buffer->attrs = (struct wlr_dmabuf_attributes){.width = info.width,
			.height = info.height, .format = drm_format, .modifier = info.modifier,
			.n_planes = 1, .offset = {info.offset}, .stride = {info.stride}, .fd = {-1}};
		buffer->attrs.fd[0] = fcntl(source_fd, F_DUPFD_CLOEXEC, 0);
		if (buffer->attrs.fd[0] < 0) { free(buffer); goto fail; }
		wlr_buffer_init(&buffer->base, &buffer_impl, info.width, info.height);
		backend->output.buffers[i] = buffer;
		buffers[i] = &buffer->base;
		if (!wlr_drm_format_set_add(&formats, drm_format, info.modifier)) goto fail;
	}
	const struct wlr_drm_format *format =
		wlr_drm_format_set_get(&formats, protocol_format_to_drm(backend->format));
	if (!format) goto fail;
	backend->output.swapchain = wlr_swapchain_create_external(backend->width,
		backend->height, format, buffers, count, select_slot, backend);
	if (!backend->output.swapchain) goto fail;
	wlr_drm_format_set_finish(&formats);
	return true;
fail:
	wlr_drm_format_set_finish(&formats);
	for (int i = 0; i < count; i++) if (buffers[i]) wlr_buffer_drop(buffers[i]);
	memset(backend->output.buffers, 0, sizeof(backend->output.buffers));
	return false;
}

static int handle_ready(int fd, uint32_t mask, void *data) {
	struct anland_backend *backend = data;
	eventfd_t value;
	if (eventfd_read(fd, &value) < 0 && errno != EAGAIN) { force_fallback(backend->display); return 0; }
	if (backend->output.inflight) {
		struct wlr_output_event_present event = {
			.commit_seq = backend->output.base.commit_seq,
			.presented = true,
		};
		clock_gettime(CLOCK_MONOTONIC, &event.when);
		wlr_output_send_present(&backend->output.base, &event);
		wlr_buffer_unlock(backend->output.inflight);
		backend->output.inflight = NULL;
	}
	if (backend->output.queued) {
		/* One frame rendered while the previous was still in flight: present it
		 * now, keeping exactly one presentation per ready signal. */
		struct wlr_buffer *queued = backend->output.queued;
		backend->output.queued = NULL;
		bool ok = present_buffer(&backend->output, queued);
		wlr_buffer_unlock(queued);
		if (!ok) { force_fallback(backend->display); return 0; }
	} else if (backend->output.inflight == NULL){
		/* The consumer just released a buffer, so this is the only moment a
		 * frame can be submitted. Damage may be empty on a static desktop, in
		 * which case the scene would skip the commit and the consumer would wait
		 * for a render-done that never comes; force one frame. */
		wlr_output_update_needs_frame(&backend->output.base);
		wlr_output_send_frame(&backend->output.base);
	}
	return 0;
}
static void emit_pointer_frame(struct anland_backend *b) {
	wl_signal_emit_mutable(&b->pointer.events.frame, &b->pointer);
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
	struct anland_clipboard_write *write = calloc(1, sizeof(*write));
	if (!write) { close(fd); return; }
	write->data = malloc(clip->size);
	if (!write->data) { close(fd); free(write); return; }
	memcpy(write->data, clip->data, clip->size);
	write->backend = backend;
	write->fd = fd;
	write->size = clip->size;
	if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) {
		close(fd); free(write->data); free(write); return;
	}
	wl_list_insert(&backend->clipboard_writes, &write->link);
	write->source = wl_event_loop_add_fd(backend->loop, fd, WL_EVENT_WRITABLE,
		handle_clipboard_write, write);
	if (!write->source) clipboard_write_destroy(write);
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

struct anland_output_message {
	struct wl_list link;
	uint8_t *data;
	size_t size, offset;
};

static void clear_output_messages(struct anland_backend *backend) {
	if (backend->output_timer) wl_event_source_remove(backend->output_timer);
	backend->output_timer = NULL;
	struct anland_output_message *message, *tmp;
	wl_list_for_each_safe(message, tmp, &backend->output_messages, link) {
		wl_list_remove(&message->link);
		free(message->data);
		free(message);
	}
}

static int flush_output_messages(void *data) {
	struct anland_backend *backend = data;
	int fd = get_data_fd(backend->display);
	while (!wl_list_empty(&backend->output_messages)) {
		struct anland_output_message *message = wl_container_of(
			backend->output_messages.next, message, link);
		ssize_t n = send(fd, message->data + message->offset,
			message->size - message->offset, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (n > 0) {
			message->offset += (size_t)n;
			if (message->offset < message->size) continue;
			wl_list_remove(&message->link);
			free(message->data);
			free(message);
			continue;
		}
		if (n < 0 && errno == EINTR) continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			wl_event_source_timer_update(backend->output_timer, 10);
			return 0;
		}
		force_fallback(backend->display);
		return 0;
	}
	if (backend->output_timer) {
		wl_event_source_remove(backend->output_timer);
		backend->output_timer = NULL;
	}
	return 0;
}

static bool queue_output_event(struct anland_backend *backend,
		const struct OutputEvent *event, const void *payload, size_t payload_size) {
	if (is_fallback(backend->display) || payload_size > INPUT_PAYLOAD_MAX)
		return false;
	struct anland_output_message *message = calloc(1, sizeof(*message));
	if (!message) return false;
	message->size = sizeof(struct data_msg) + sizeof(*event) + payload_size;
	message->data = malloc(message->size);
	if (!message->data) { free(message); return false; }
	struct data_msg header = {
		.type = DATA_MSG_OUTPUT_EVENT,
		.size = sizeof(*event),
	};
	memcpy(message->data, &header, sizeof(header));
	memcpy(message->data + sizeof(header), event, sizeof(*event));
	if (payload_size)
		memcpy(message->data + sizeof(header) + sizeof(*event), payload,
			payload_size);
	wl_list_insert(backend->output_messages.prev, &message->link);
	if (!backend->output_timer) {
		backend->output_timer = wl_event_loop_add_timer(backend->loop,
			flush_output_messages, backend);
		if (!backend->output_timer) {
			clear_output_messages(backend);
			return false;
		}
	}
	wl_event_source_timer_update(backend->output_timer, 0);
	return true;
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
	if (transfer->size > 0 && !is_fallback(transfer->backend->display)) {
		struct OutputEvent ev = {.type = OUTPUT_TYPE_CLIPBOARD};
		ev.clipboard.size = transfer->size;
		queue_output_event(transfer->backend, &ev,
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
			is_fallback(backend->display)) return;
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
	struct anland_backend *backend = wl_container_of(listener, backend,
		selection_listener);
	send_selection_to_consumer(backend,
		server.seat ? server.seat->selection_source : NULL);
}

static void clear_input_payload(struct anland_backend *backend) {
	free(backend->input_payload);
	backend->input_payload = NULL;
	backend->input_payload_size = 0;
	backend->input_payload_offset = 0;
	backend->input_payload_type = 0;
	backend->input_header_offset = 0;
}

static bool drain_input_payload(struct anland_backend *backend, int fd) {
	while (backend->input_payload_offset < backend->input_payload_size) {
		ssize_t n = recv(fd, backend->input_payload + backend->input_payload_offset,
			backend->input_payload_size - backend->input_payload_offset, MSG_DONTWAIT);
		if (n > 0) { backend->input_payload_offset += (size_t)n; continue; }
		if (n == 0) { force_fallback(backend->display); return false; }
		if (errno == EINTR) continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
		force_fallback(backend->display); return false;
	}
	backend->input_payload[backend->input_payload_size] = '\0';
	if (backend->input_payload_type == INPUT_TYPE_TEXT_INPUT)
		mango_text_input_commit_utf8(server.input_method_relay,
			backend->input_payload, backend->input_payload_size);
	else
		set_clipboard_from_consumer(backend, backend->input_payload,
			backend->input_payload_size);
	clear_input_payload(backend);
	return true;
}

static int receive_input_event(struct anland_backend *backend, int fd,
		struct InputEvent *event) {
	while (backend->input_header_offset < sizeof(backend->input_header)) {
		ssize_t n = recv(fd,
			backend->input_header + backend->input_header_offset,
			sizeof(backend->input_header) - backend->input_header_offset,
			MSG_DONTWAIT);
		if (n > 0) { backend->input_header_offset += (size_t)n; continue; }
		if (n == 0) { force_fallback(backend->display); return -1; }
		if (errno == EINTR) continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
		force_fallback(backend->display);
		return -1;
	}
	struct data_msg header;
	memcpy(&header, backend->input_header, sizeof(header));
	if (header.type != DATA_MSG_INPUT_EVENT ||
			header.size != sizeof(struct InputEvent)) {
		force_fallback(backend->display);
		return -1;
	}
	memcpy(event, backend->input_header + sizeof(header), sizeof(*event));
	backend->input_header_offset = 0;
	return 1;
}

static int handle_input(int fd, uint32_t mask, void *data) {
	struct anland_backend *b = data;
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		force_fallback(b->display); return 0;
	}
	if (b->input_payload && !drain_input_payload(b, fd)) return 0;
	struct InputEvent ev;
	int ret;
	while ((ret = receive_input_event(b, fd, &ev)) > 0) {
		uint32_t time = now_msec();
		switch (ev.type) {
		case INPUT_TYPE_KEY: {
			struct wlr_keyboard_key_event e = {.time_msec=time, .keycode=ev.key.keycode,
				.update_state=true, .state=ev.key.action == INPUT_ACTION_DOWN};
			wlr_keyboard_notify_key(&b->keyboard, &e); break;
		}
		case INPUT_TYPE_POINTER_MOTION: {
			struct wlr_pointer_motion_absolute_event a = {.pointer=&b->pointer,
				.time_msec=time, .x=normalized(ev.pointer_motion.x,b->width),
				.y=normalized(ev.pointer_motion.y,b->height)};
			wl_signal_emit_mutable(&b->pointer.events.motion_absolute,&a);
			struct wlr_pointer_motion_event r = {.pointer=&b->pointer,.time_msec=time,
				.delta_x=ev.pointer_motion.dx,.delta_y=ev.pointer_motion.dy,
				.unaccel_dx=ev.pointer_motion.dx,.unaccel_dy=ev.pointer_motion.dy};
			wl_signal_emit_mutable(&b->pointer.events.motion,&r); emit_pointer_frame(b); break;
		}
		case INPUT_TYPE_POINTER_BUTTON: {
			struct wlr_pointer_button_event e = {.pointer=&b->pointer,.time_msec=time,
				.button=ev.pointer_button.button,.state=ev.pointer_button.pressed};
			wlr_pointer_notify_button(&b->pointer,&e); emit_pointer_frame(b); break;
		}
		case INPUT_TYPE_POINTER_AXIS: {
			struct wlr_pointer_axis_event e = {.pointer=&b->pointer,.time_msec=time,
				.source=WL_POINTER_AXIS_SOURCE_WHEEL,
				.orientation=ev.pointer_axis.axis == 0 ? WL_POINTER_AXIS_VERTICAL_SCROLL : WL_POINTER_AXIS_HORIZONTAL_SCROLL,
				.relative_direction=WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
				.delta=ev.pointer_axis.value,
				.delta_discrete=ev.pointer_axis.discrete * WLR_POINTER_AXIS_DISCRETE_STEP};
			wl_signal_emit_mutable(&b->pointer.events.axis,&e); emit_pointer_frame(b); break;
		}
		case INPUT_TYPE_TOUCH: {
			double x=normalized(ev.touch.x,b->width), y=normalized(ev.touch.y,b->height);
			if (ev.touch.action == INPUT_ACTION_DOWN) { struct wlr_touch_down_event e={.touch=&b->touch,.time_msec=time,.touch_id=ev.touch.pointer_id,.x=x,.y=y}; wl_signal_emit_mutable(&b->touch.events.down,&e); }
			else if (ev.touch.action == INPUT_ACTION_MOVE) { struct wlr_touch_motion_event e={.touch=&b->touch,.time_msec=time,.touch_id=ev.touch.pointer_id,.x=x,.y=y}; wl_signal_emit_mutable(&b->touch.events.motion,&e); }
			else { struct wlr_touch_up_event e={.touch=&b->touch,.time_msec=time,.touch_id=ev.touch.pointer_id}; wl_signal_emit_mutable(&b->touch.events.up,&e); } break;
		}
		case INPUT_TYPE_TOUCH_FRAME: wl_signal_emit_mutable(&b->touch.events.frame,&b->touch); break;
		case INPUT_TYPE_DISPLAY_REFRESH:
			if (ev.display.refresh_mhz && ev.display.refresh_mhz != b->refresh) {
				b->refresh=ev.display.refresh_mhz; struct wlr_output_state state;
				wlr_output_state_init(&state); wlr_output_state_set_custom_mode(&state,b->width,b->height,b->refresh);
				wlr_output_send_request_state(&b->output.base,&state); wlr_output_state_finish(&state);
			} break;
		case INPUT_TYPE_CLIPBOARD: case INPUT_TYPE_TEXT_INPUT: {
			size_t size = ev.type == INPUT_TYPE_CLIPBOARD ? ev.clipboard.size : ev.text_input.size;
			if (size == 0 || size > INPUT_PAYLOAD_MAX) { force_fallback(b->display); return 0; }
			b->input_payload = malloc(size + 1);
			if (!b->input_payload) { force_fallback(b->display); return 0; }
			b->input_payload_size = size;
			b->input_payload_type = ev.type;
			if (!drain_input_payload(b, fd)) return 0;
			break;
		}
		default: break;
		}
	}
	return 0;
}
static bool attach_sources(struct anland_backend *b) {
	int data_fd = get_data_fd(b->display);
	int flags = fcntl(data_fd, F_GETFL);
	if (flags < 0 || fcntl(data_fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return false;
	b->data_source=wl_event_loop_add_fd(b->loop,data_fd,WL_EVENT_READABLE,handle_input,b);
	b->ready_source=wl_event_loop_add_fd(b->loop,get_buffer_ready_fd(b->display),WL_EVENT_READABLE,handle_ready,b);
	return b->data_source && b->ready_source;
}
static void announce_devices(struct anland_backend *b) {
	if (b->announced) return;
	b->announced = true;
	wl_signal_emit_mutable(&b->base.events.new_output,&b->output.base);
	wl_signal_emit_mutable(&b->base.events.new_input,&b->keyboard.base);
	wl_signal_emit_mutable(&b->base.events.new_input,&b->pointer.base);
	wl_signal_emit_mutable(&b->base.events.new_input,&b->touch.base);
}
static bool reconnect_daemon(struct anland_backend *b) {
	if (b->display && is_daemon_alive(b->display)) return true;
	if (b->display) {
		disconnect(b->display);
		b->display = NULL;
	}
	if (connect_to_deamon(&b->display, b->socket_path) < 0)
		return false;
	if (get_screen_info(b->display, &b->width, &b->height, &b->format,
			&b->refresh) < 0 || !b->width || !b->height) {
		disconnect(b->display);
		b->display = NULL;
		return false;
	}
	set_pre_release_callback(b->display, producer_pre_release, b);
	set_fallback_callback(b->display, producer_fallback, b);
	return true;
}
static int reconnect(void *data) {
	struct anland_backend *b=data;
	if (!reconnect_daemon(b) || try_exit_fallback(b->display)<0) {
		wl_event_source_timer_update(b->reconnect_source,RECONNECT_MS);
		return 0;
	}
	if (!import_buffers(b) || !attach_sources(b)) {
		force_fallback(b->display);
		return 0;
	}
	anland_audio_set_fd(get_audio_fd(b->display));
	announce_devices(b);
	wlr_output_update_needs_frame(&b->output.base);
	wlr_output_send_frame(&b->output.base);
	return 0;
}

/* Hand a rendered buffer to the consumer: export its render fence so
 * SurfaceFlinger waits GPU-side, signal the frame with trigger_refresh(), and
 * take the reference the output keeps until the consumer reports it ready. */
static bool present_buffer(struct anland_output *output, struct wlr_buffer *buffer) {
	struct wlr_dmabuf_attributes attrs;
	if (wlr_buffer_get_dmabuf(buffer,&attrs)) {
		struct dma_buf_export_sync_file sync={.flags=DMA_BUF_SYNC_WRITE,.fd=-1};
		if (ioctl(attrs.fd[0],DMA_BUF_IOCTL_EXPORT_SYNC_FILE,&sync)==0)
			set_render_fence(output->backend->display,sync.fd);
	}
	if (trigger_refresh(output->backend->display)<0)
		return false;
	output->inflight=wlr_buffer_lock(buffer);
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
			(!output->swapchain || !state->buffer ||
			!wlr_swapchain_has_buffer(output->swapchain, state->buffer))) {
		return false;
	}
	return true;
}
static bool output_commit(struct wlr_output *base,
		const struct wlr_output_state *state) {
	struct anland_output *output=wl_container_of(base,output,base);
	if (!output_test(base,state)) return false;
	if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
		/* Newest queued frame wins: a frame that was never presented was drawn
		 * from the same consumer state, so the later one supersedes it. */
		if (output->queued) {
			wlr_buffer_unlock(output->queued);
			output->queued = NULL;
		}
		if (output->inflight) {
			/* The consumer has not reported the previous buffer ready yet. The
			 * compositor may legitimately commit twice between two ready signals
			 * (an idle frame event can slip in), so queue this frame instead of
			 * rejecting the commit. */
			output->queued=wlr_buffer_lock(state->buffer);
			return true;
		}
		if (!present_buffer(output, state->buffer))
			return false;
	}
	return true;
}
static void output_destroy(struct wlr_output *base) {
	struct anland_output *output=wl_container_of(base,output,base);
	drop_buffers(output->backend); wlr_output_finish(base);
}
static const struct wlr_output_impl output_impl={.destroy=output_destroy,.test=output_test,.commit=output_commit};
bool mango_anland_output_is(struct wlr_output *output) { return output && output->impl==&output_impl; }
struct wlr_swapchain *mango_anland_output_swapchain(struct wlr_output *output) {
	if (!mango_anland_output_is(output)) return NULL;
	struct anland_output *anland=wl_container_of(output,anland,base); return anland->swapchain;
}
bool mango_anland_touch_is(struct wlr_touch *touch) {
	return touch && touch->impl == &touch_impl;
}
static bool backend_start(struct wlr_backend *base) {
	struct anland_backend *b=wl_container_of(base,b,base);
	if (anland_audio_start() == 0)
		b->audio_started = true;
	if (server.seat && !b->selection_listener_set) {
		b->selection_listener.notify = handle_seat_set_selection;
		wl_signal_add(&server.seat->events.set_selection, &b->selection_listener);
		b->selection_listener_set = true;
	}
	reconnect(b);
	return true;
}
static void backend_destroy(struct wlr_backend *base) {
	struct anland_backend *b=wl_container_of(base,b,base);
	if (b->selection_listener_set) wl_list_remove(&b->selection_listener.link);
	if (b->reconnect_source) wl_event_source_remove(b->reconnect_source);
	b->reconnect_source = NULL;
	producer_pre_release(b);
	if (b->audio_started) anland_audio_stop();
	if (b->display) disconnect(b->display);
	if (b->drm_fd>=0) close(b->drm_fd);
	if (b->output_initialized) wlr_output_destroy(&b->output.base);
	wlr_keyboard_finish(&b->keyboard);
	wlr_pointer_finish(&b->pointer);
	wlr_touch_finish(&b->touch);
	wlr_backend_finish(base);
	free(b->socket_path);
	free(b);
}
static int backend_get_drm_fd(struct wlr_backend *base) {
	struct anland_backend *b=wl_container_of(base,b,base);
	return b->drm_fd;
}
static const struct wlr_backend_impl backend_impl={.start=backend_start,.destroy=backend_destroy,.get_drm_fd=backend_get_drm_fd};
struct wlr_backend *mango_anland_backend_create(struct wl_event_loop *loop,const char *socket_path) {
	struct anland_backend *b=calloc(1,sizeof(*b)); if(!b) return NULL;
	b->socket_path = strdup(socket_path ? socket_path : "");
	if (!b->socket_path) { free(b); return NULL; }
	b->loop=loop; b->drm_fd=-1;
	wl_list_init(&b->clipboard_writes);
	wl_list_init(&b->output_messages);
	const char *drm_path=getenv("ANLAND_DRM_DEVICE");
	if(!drm_path||!*drm_path) drm_path=getenv("WLR_RENDER_DRM_DEVICE");
	if(drm_path&&*drm_path) {
		b->drm_fd=open(drm_path,O_RDWR|O_CLOEXEC);
		if(b->drm_fd<0) goto fail;
	}
	if(connect_to_deamon(&b->display,socket_path)<0 ||
			get_screen_info(b->display,&b->width,&b->height,&b->format,&b->refresh)<0 ||
			!b->width || !b->height) goto fail;
	wlr_backend_init(&b->base,&backend_impl);
	b->base.buffer_caps=WLR_BUFFER_CAP_DMABUF;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state,b->width,b->height,b->refresh);
	b->output.backend=b;
	wlr_output_init(&b->output.base,&b->base,&output_impl,loop,&state);
	b->output_initialized = true;
	wlr_output_state_finish(&state);
	wlr_output_set_name(&b->output.base,"ANLAND-1");
	wlr_output_set_description(&b->output.base,"Anland 5 display");
	wlr_keyboard_init(&b->keyboard,&keyboard_impl,"Anland keyboard");
	wlr_pointer_init(&b->pointer,&pointer_impl,"Anland pointer");
	wlr_touch_init(&b->touch,&touch_impl,"Anland touch");
	b->reconnect_source=wl_event_loop_add_timer(loop,reconnect,b);
	if(!b->reconnect_source) goto fail_initialized;
	set_pre_release_callback(b->display,producer_pre_release,b);
	set_fallback_callback(b->display,producer_fallback,b);
	return &b->base;
fail_initialized:
	wlr_output_destroy(&b->output.base);
	wlr_keyboard_finish(&b->keyboard);
	wlr_pointer_finish(&b->pointer);
	wlr_touch_finish(&b->touch);
	wlr_backend_finish(&b->base);
fail:
	if(b->display) disconnect(b->display);
	if(b->drm_fd>=0) close(b->drm_fd);
	free(b->socket_path);
	free(b);
	return NULL;
}
#endif
