#define _GNU_SOURCE
#define HAVE_ANLAND 1

#include "mango/common/server.h"
#include "mango/ext-protocol/text-input.h"
#include <anland_device.h>
#include <assert.h>
#include <errno.h>
#include <string.h>

struct MangoServer server;

static int forced_ioctl_errno;
static int input_read_calls;
static int committed_payloads;
static char committed_payload[32];
static void (*device_pre_release_cb)(void *);
static void *device_pre_release_data;
static bool clipboard_callback_saw_cleared_read;

static int smoke_ioctl(int fd, unsigned long request, ...) {
	(void)fd;
	(void)request;
	errno = forced_ioctl_errno;
	return -1;
}

static int smoke_device_is_connected(anland_device *device) {
	(void)device;
	return 1;
}

static int smoke_device_set_clipboard(anland_device *device,
		const void *text, size_t size);

static int smoke_device_read_input(anland_device *device, void *buffer,
		size_t size, int timeout_ms) {
	(void)device;
	(void)timeout_ms;
	input_read_calls++;
	if (input_read_calls == 1)
		return 0;
	assert(size == strlen("partial-ok"));
	memcpy(buffer, "partial-ok", size);
	return 1;
}

#define ioctl smoke_ioctl
#define anland_device_is_connected smoke_device_is_connected
#define anland_device_set_clipboard smoke_device_set_clipboard
#define anland_device_read_input smoke_device_read_input

bool mango_text_input_commit_utf8(struct mango_input_method_relay *relay,
		const char *text, size_t length) {
	(void)relay;
	assert(length < sizeof(committed_payload));
	memcpy(committed_payload, text, length);
	committed_payload[length] = '\0';
	committed_payloads++;
	return true;
}

#include "../src/backend/anland.c"

#undef anland_device_read_input
#undef anland_device_set_clipboard
#undef anland_device_is_connected
#undef ioctl

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>

static int smoke_device_set_clipboard(anland_device *device,
		const void *text, size_t size) {
	(void)device;
	assert(text != NULL && size > 0);
	struct anland_backend *backend = device_pre_release_data;
	clipboard_callback_saw_cleared_read = backend->clipboard_read == NULL;
	assert(device_pre_release_cb != NULL);
	device_pre_release_cb(device_pre_release_data);
	return -1;
}

struct counts {
	int key_release;
	int button_release;
	int pointer_frame;
	int touch_cancel;
	int touch_frame;
	struct wl_listener key;
	struct wl_listener button;
	struct wl_listener pframe;
	struct wl_listener cancel;
	struct wl_listener tframe;
};

static void on_key(struct wl_listener *listener, void *data) {
	struct counts *counts = wl_container_of(listener, counts, key);
	struct wlr_keyboard_key_event *event = data;
	if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED)
		counts->key_release++;
}

static void on_button(struct wl_listener *listener, void *data) {
	struct counts *counts = wl_container_of(listener, counts, button);
	struct wlr_pointer_button_event *event = data;
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED)
		counts->button_release++;
}

static void on_pointer_frame(struct wl_listener *listener, void *data) {
	(void)data;
	struct counts *counts = wl_container_of(listener, counts, pframe);
	counts->pointer_frame++;
}

static void on_touch_cancel(struct wl_listener *listener, void *data) {
	(void)data;
	struct counts *counts = wl_container_of(listener, counts, cancel);
	counts->touch_cancel++;
}

static void on_touch_frame(struct wl_listener *listener, void *data) {
	(void)data;
	struct counts *counts = wl_container_of(listener, counts, tframe);
	counts->touch_frame++;
}

static int ignored_timer(void *data) {
	(void)data;
	return 0;
}

int main(void) {
	signal(SIGPIPE, SIG_IGN);
	server.display = wl_display_create();
	assert(server.display != NULL);
	server.event_loop = wl_display_get_event_loop(server.display);

	struct anland_backend backend = {0};
	backend.loop = server.event_loop;
	backend.device = (anland_device *)(uintptr_t)0x1;
	wl_list_init(&backend.clipboard_writes);
	wlr_keyboard_init(&backend.keyboard, &keyboard_impl, "test-keyboard");
	wlr_pointer_init(&backend.pointer, &pointer_impl, "test-pointer");
	wlr_touch_init(&backend.touch, &touch_impl, "test-touch");

	struct counts counts = {0};
	counts.key.notify = on_key;
	counts.button.notify = on_button;
	counts.pframe.notify = on_pointer_frame;
	counts.cancel.notify = on_touch_cancel;
	counts.tframe.notify = on_touch_frame;
	wl_signal_add(&backend.keyboard.events.key, &counts.key);
	wl_signal_add(&backend.pointer.events.button, &counts.button);
	wl_signal_add(&backend.pointer.events.frame, &counts.pframe);
	wl_signal_add(&backend.touch.events.cancel, &counts.cancel);
	wl_signal_add(&backend.touch.events.frame, &counts.tframe);

	struct wlr_keyboard_key_event key = {
		.time_msec = 1,
		.keycode = 30,
		.update_state = true,
		.state = WL_KEYBOARD_KEY_STATE_PRESSED,
	};
	wlr_keyboard_notify_key(&backend.keyboard, &key);
	backend.pressed_buttons[0] = 272;
	backend.pressed_buttons[1] = 273;
	backend.pressed_button_count = 2;
	backend.touch_ids[0] = 4;
	backend.touch_ids[1] = 7;
	backend.touch_count = 2;
	reset_input(&backend);
	assert(counts.key_release == 1);
	assert(counts.button_release == 2 && counts.pointer_frame == 1);
	assert(counts.touch_cancel == 2 && counts.touch_frame == 1);
	assert(backend.keyboard.num_keycodes == 0);
	assert(backend.pressed_button_count == 0 && backend.touch_count == 0);

	struct anland_clipboard_source clip = {0};
	clip.backend = &backend;
	clip.data = strdup("test-clipboard");
	clip.size = strlen(clip.data);
	wlr_data_source_init(&clip.source, &clipboard_source_impl);
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
	close(sockets[0]);
	clipboard_source_send(&clip.source, CLIPBOARD_MIME_UTF8, sockets[1]);
	assert(!wl_list_empty(&backend.clipboard_writes));
	assert(wl_event_loop_dispatch(server.event_loop, 0) == 0);
	assert(wl_list_empty(&backend.clipboard_writes));
	free(clip.data);

	struct anland_clipboard_read *read = calloc(1, sizeof(*read));
	assert(read != NULL);
	read->backend = &backend;
	read->fd = -1;
	read->data = strdup("consumer-selection");
	assert(read->data != NULL);
	read->size = strlen(read->data);
	read->capacity = read->size;
	backend.clipboard_read = read;
	device_pre_release_cb = producer_pre_release;
	device_pre_release_data = &backend;
	finish_clipboard_read(read);
	assert(clipboard_callback_saw_cleared_read);
	assert(backend.clipboard_read == NULL && backend.session_lost);

	backend.session_lost = false;
	backend.input_source = wl_event_loop_add_timer(server.event_loop,
		ignored_timer, NULL);
	assert(backend.input_source != NULL);
	backend.input_payload_size = strlen("partial-ok");
	backend.input_payload = malloc(backend.input_payload_size + 1);
	assert(backend.input_payload != NULL);
	backend.pending_event.type = ANLAND_DEVICE_IN_TEXT_INPUT;
	backend.pending_input = PENDING_INPUT_PAYLOAD;
	backend.input_deadline_msec = UINT64_MAX;
	assert(progress_pending_input(&backend) == 0);
	assert(backend.pending_input == PENDING_INPUT_PAYLOAD);
	assert(backend.input_payload != NULL && committed_payloads == 0);
	assert(progress_pending_input(&backend) == 1);
	assert(backend.pending_input == PENDING_INPUT_NONE);
	assert(backend.input_payload == NULL && committed_payloads == 1);
	assert(strcmp(committed_payload, "partial-ok") == 0);
	wl_event_source_remove(backend.input_source);
	backend.input_source = NULL;

	int release1[2], release2[2];
	assert(pipe2(release1, O_CLOEXEC) == 0);
	assert(pipe2(release2, O_CLOEXEC) == 0);
	anland_scene_event_t events[3] = {
		{.type = ANLAND_SCENE_EVENT_PRESENTED, .commit_id = 99},
		{.type = ANLAND_SCENE_EVENT_BUFFER_RELEASED,
			.u.released.release_fence_fd = release1[0]},
		{.type = ANLAND_SCENE_EVENT_BUFFER_RELEASED,
			.u.released.release_fence_fd = release2[0]},
	};
	bool target_ready = false;
	assert(!handle_scene_event_batch(&backend, events, 3, &target_ready));
	errno = 0;
	assert(fcntl(release1[0], F_GETFD) == -1 && errno == EBADF);
	errno = 0;
	assert(fcntl(release2[0], F_GETFD) == -1 && errno == EBADF);
	close(release1[1]);
	close(release2[1]);

	struct anland_buffer fence_buffer = {0};
	fence_buffer.attrs.n_planes = 1;
	fence_buffer.attrs.fd[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
	assert(fence_buffer.attrs.fd[0] >= 0);
	wlr_buffer_init(&fence_buffer.base, &buffer_impl, 1, 1);
	int exported_fd = 123;
	forced_ioctl_errno = ENOTTY;
	assert(export_write_fence(&fence_buffer.base, &exported_fd));
	assert(exported_fd == -1);
	forced_ioctl_errno = EINVAL;
	assert(!export_write_fence(&fence_buffer.base, &exported_fd));
	assert(exported_fd == -1);
	close(fence_buffer.attrs.fd[0]);

	printf("PASS anland_backend reset batch_fd einval clipboard_reentry partial_zero\n");

	wl_list_remove(&counts.key.link);
	wl_list_remove(&counts.button.link);
	wl_list_remove(&counts.pframe.link);
	wl_list_remove(&counts.cancel.link);
	wl_list_remove(&counts.tframe.link);
	wlr_touch_finish(&backend.touch);
	wlr_pointer_finish(&backend.pointer);
	wlr_keyboard_finish(&backend.keyboard);
	wl_display_destroy(server.display);
	return 0;
}
