#define _GNU_SOURCE
#define HAVE_ANLAND 1

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>

#include "mango/common/server.h"

struct MangoServer server;
static struct wl_client *client_a = (struct wl_client *)(uintptr_t)0x11;
static struct wl_client *client_b = (struct wl_client *)(uintptr_t)0x22;
static int commits;
static int text_input_dones;
static int enters;
static int leaves;
static int activates;
static int deactivates;
static int ime_dones;
static int surrounding_sends;
static int cause_sends;
static int content_type_sends;
static int unavailable_sends;
static char committed[64];
static char surrounding[64];
static uint32_t surrounding_cursor;
static uint32_t surrounding_anchor;
static uint32_t sent_cause;
static uint32_t sent_hint;
static uint32_t sent_purpose;

static struct wl_client *test_resource_client(struct wl_resource *resource) {
	return resource ? resource->data : NULL;
}
#define wl_resource_get_client test_resource_client

static void test_text_commit(struct wlr_text_input_v3 *input,
		const char *text) {
	(void)input;
	commits++;
	snprintf(committed, sizeof(committed), "%s", text);
}

static void test_text_done(struct wlr_text_input_v3 *input) {
	(void)input;
	text_input_dones++;
}

static void test_enter(struct wlr_text_input_v3 *input,
		struct wlr_surface *surface) {
	enters++;
	input->focused_surface = surface;
}

static void test_leave(struct wlr_text_input_v3 *input) {
	leaves++;
	input->focused_surface = NULL;
}

static void test_activate(struct wlr_input_method_v2 *input_method) {
	(void)input_method;
	activates++;
}

static void test_deactivate(struct wlr_input_method_v2 *input_method) {
	(void)input_method;
	deactivates++;
}

static void test_surrounding(struct wlr_input_method_v2 *input_method,
		const char *text, uint32_t cursor, uint32_t anchor) {
	(void)input_method;
	surrounding_sends++;
	snprintf(surrounding, sizeof(surrounding), "%s", text);
	surrounding_cursor = cursor;
	surrounding_anchor = anchor;
}

static void test_content_type(struct wlr_input_method_v2 *input_method,
		uint32_t hint, uint32_t purpose) {
	(void)input_method;
	content_type_sends++;
	sent_hint = hint;
	sent_purpose = purpose;
}

static void test_cause(struct wlr_input_method_v2 *input_method,
		uint32_t cause) {
	(void)input_method;
	cause_sends++;
	sent_cause = cause;
}

static void test_ime_done(struct wlr_input_method_v2 *input_method) {
	(void)input_method;
	ime_dones++;
}

static void test_unavailable(struct wlr_input_method_v2 *input_method) {
	(void)input_method;
	unavailable_sends++;
}

#define wlr_text_input_v3_send_enter test_enter
#define wlr_text_input_v3_send_leave test_leave
#define wlr_text_input_v3_send_commit_string test_text_commit
#define wlr_text_input_v3_send_done test_text_done
#define wlr_input_method_v2_send_activate test_activate
#define wlr_input_method_v2_send_deactivate test_deactivate
#define wlr_input_method_v2_send_surrounding_text test_surrounding
#define wlr_input_method_v2_send_content_type test_content_type
#define wlr_input_method_v2_send_text_change_cause test_cause
#define wlr_input_method_v2_send_done test_ime_done
#define wlr_input_method_v2_send_unavailable test_unavailable

static bool test_backend_is(struct wlr_backend *backend) {
	return backend == (struct wlr_backend *)(uintptr_t)0xa11a;
}
#define mango_anland_backend_is test_backend_is

void *ecalloc(size_t nmemb, size_t size) {
	void *result = calloc(nmemb, size);
	assert(result != NULL);
	return result;
}

Monitor *get_monitor_nearest_to(int32_t lx, int32_t ly) {
	(void)lx;
	(void)ly;
	return NULL;
}

#include "../src/ext-protocol/text-input.c"

static struct wl_resource resource_for(struct wl_client *client) {
	struct wl_resource resource = {0};
	resource.data = client;
	return resource;
}

static void surface_init(struct wlr_surface *surface,
		struct wl_resource *resource) {
	memset(surface, 0, sizeof(*surface));
	surface->resource = resource;
	wl_signal_init(&surface->events.destroy);
}

static void input_method_init(struct wlr_input_method_v2 *input_method,
		struct wlr_seat *seat) {
	memset(input_method, 0, sizeof(*input_method));
	input_method->seat = seat;
	wl_signal_init(&input_method->events.commit);
	wl_signal_init(&input_method->events.new_popup_surface);
	wl_signal_init(&input_method->events.grab_keyboard);
	wl_signal_init(&input_method->events.destroy);
}

static void assert_full_state_sent(int expected_count) {
	assert(activates == expected_count);
	assert(ime_dones == expected_count);
	assert(surrounding_sends == expected_count);
	assert(cause_sends == expected_count);
	assert(content_type_sends == expected_count);
	assert(strcmp(surrounding, "editor state") == 0);
	assert(surrounding_cursor == 6 && surrounding_anchor == 2);
	assert(sent_cause == 9 && sent_hint == 17 && sent_purpose == 23);
}

int main(void) {
	struct wlr_seat seat = {0};
	struct wlr_surface surface_a, surface_b;
	struct wl_resource resource_a = resource_for(client_a);
	struct wl_resource resource_b = resource_for(client_b);
	struct wlr_text_input_v3 input = {0};
	struct text_input text_input = {0};
	struct mango_input_method_relay relay = {0};
	struct wlr_backend *ordinary = (struct wlr_backend *)(uintptr_t)0xb00b;

	wl_signal_init(&seat.keyboard_state.events.focus_change);
	server.seat = &seat;
	surface_init(&surface_a, &resource_a);
	surface_init(&surface_b, &resource_b);
	input.resource = &resource_a;
	input.seat = &seat;
	input.current_enabled = true;
	input.active_features = WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT |
		WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE;
	input.current.surrounding.text = "editor state";
	input.current.surrounding.cursor = 6;
	input.current.surrounding.anchor = 2;
	input.current.text_change_cause = 9;
	input.current.content_type.hint = 17;
	input.current.content_type.purpose = 23;
	text_input.input = &input;
	text_input.relay = &relay;
	wl_list_init(&relay.text_inputs);
	wl_list_init(&relay.popups);
	wl_list_insert(&relay.text_inputs, &text_input.link);
	relay.focused_surface = &surface_a;
	relay.new_input_method.notify = handle_new_input_method;
	server.input_method_relay = &relay;
	seat.keyboard_state.focused_surface = &surface_a;

	server.backend = ordinary;
	relay.direct_mode = test_backend_is(server.backend);
	assert(!relay.direct_mode && relay.active_text_input == NULL);
	assert(enters == 0 && input.focused_surface == NULL);
	assert(!mango_text_input_commit_utf8(&relay, "ignored", 7));

	struct wlr_input_method_v2 input_method1;
	input_method_init(&input_method1, &seat);
	handle_new_input_method(&relay.new_input_method, &input_method1);
	assert(relay.active_text_input == &text_input && enters == 1);
	assert_full_state_sent(1);
	assert(deactivates == 0 && unavailable_sends == 0);

	handle_input_method_destroy(&relay.input_method_destroy, NULL);
	assert(relay.input_method == NULL && relay.active_text_input == NULL);
	assert(input.focused_surface == NULL && leaves == 1);

	server.backend = (struct wlr_backend *)(uintptr_t)0xa11a;
	relay.direct_mode = test_backend_is(server.backend);
	update_text_inputs_focused_surface(&relay);
	update_active_text_input(&relay);
	assert(relay.direct_mode && relay.active_text_input == &text_input);
	assert(enters == 2);
	assert(mango_text_input_commit_utf8(&relay, "中文", strlen("中文")));
	assert(commits == 1 && text_input_dones == 1);
	assert(strcmp(committed, "中文") == 0);

	seat.keyboard_state.focused_surface = &surface_b;
	assert(!mango_text_input_commit_utf8(&relay, "拒绝", strlen("拒绝")));
	seat.keyboard_state.focused_surface = &surface_a;
	input.current_enabled = false;
	assert(!mango_text_input_commit_utf8(&relay, "拒绝", strlen("拒绝")));
	input.current_enabled = true;

	struct wlr_input_method_v2 input_method2;
	input_method_init(&input_method2, &seat);
	handle_new_input_method(&relay.new_input_method, &input_method2);
	assert(relay.active_text_input == &text_input);
	assert_full_state_sent(2);
	assert(deactivates == 0 && unavailable_sends == 0);

	struct wlr_input_method_keyboard_grab_v2 grab = {0};
	input_method2.keyboard_grab = &grab;
	assert(!mango_text_input_commit_utf8(&relay, "拒绝", strlen("拒绝")));
	input_method2.keyboard_grab = NULL;
	handle_input_method_destroy(&relay.input_method_destroy, NULL);

	printf("PASS text_input ordinary_ime_once direct_commit ime_restart_full_state\n");
	return 0;
}
