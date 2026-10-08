#ifndef MANGO_BACKEND_ANLAND_H
#define MANGO_BACKEND_ANLAND_H

#include <stdbool.h>
#include <stddef.h>

struct wl_event_loop;
struct wlr_backend;
struct wlr_output;
struct wlr_swapchain;
struct wlr_touch;

#ifdef HAVE_ANLAND
bool mango_anland_backend_is(struct wlr_backend *backend);
struct wlr_backend *mango_anland_backend_create(struct wl_event_loop *loop,
	const char *socket_path);
bool mango_anland_output_is(struct wlr_output *output);
struct wlr_swapchain *mango_anland_output_swapchain(struct wlr_output *output);
bool mango_anland_touch_is(struct wlr_touch *touch);
#else
static inline bool mango_anland_backend_is(struct wlr_backend *backend) {
	(void)backend;
	return false;
}
static inline bool mango_anland_output_is(struct wlr_output *output) {
	(void)output;
	return false;
}
static inline struct wlr_swapchain *mango_anland_output_swapchain(
		struct wlr_output *output) {
	(void)output;
	return NULL;
}
static inline bool mango_anland_touch_is(struct wlr_touch *touch) {
	(void)touch;
	return false;
}
#endif

#endif
