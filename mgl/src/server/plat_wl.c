/* mgl-server platform for the TV: each EGL window surface is a wl_shell window
 * on the Tizen compositor (E20), rendered by the Mali through wayland-egl, so
 * frames reach the screen with no CPU copy. The TV's libwayland-client is
 * ~1.10, so requests use wl_proxy_marshal(_constructor) directly.
 * Env: MGL_OUTPUT_SIZE=WxH scales windows to that size via wl_scaler. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <wayland-client-core.h>
#include <wayland-egl-core.h>

#include "server.h"

extern const struct wl_interface wl_registry_interface, wl_compositor_interface, wl_surface_interface,
    wl_shell_interface, wl_shell_surface_interface;

/* The old wl_scaler / wl_viewport protocol (Weston scaler.xml v2), which E20 offers. */
extern const struct wl_interface wl_viewport_iface;
static const struct wl_interface *scaler_types[] = {&wl_viewport_iface, &wl_surface_interface};
static const struct wl_interface *null6[] = {NULL, NULL, NULL, NULL, NULL, NULL};
static const struct wl_message scaler_requests[] = {{"destroy", "", null6}, {"get_viewport", "no", scaler_types}};
static const struct wl_interface wl_scaler_iface = {"wl_scaler", 2, 2, scaler_requests, 0, NULL};
static const struct wl_message viewport_requests[] = {
	{"destroy", "", null6}, {"set", "ffffii", null6}, {"set_source", "2ffff", null6}, {"set_destination", "2ii", null6}};
const struct wl_interface wl_viewport_iface = {"wl_viewport", 2, 4, viewport_requests, 0, NULL};

static struct wl_display *display;
static struct wl_proxy *compositor, *shell, *scaler;
static int out_w, out_h;

struct win {
	struct wl_proxy *surface, *shell_surface, *viewport;
	struct wl_egl_window *egl;
};

static void reg_global(void *d, struct wl_proxy *reg, uint32_t name, const char *iface, uint32_t ver) {
	(void)d, (void)ver;
	if (!strcmp(iface, "wl_compositor"))
		compositor = wl_proxy_marshal_constructor_versioned(reg, 0, &wl_compositor_interface, 3, name, "wl_compositor", 3, NULL);
	else if (!strcmp(iface, "wl_shell"))
		shell = wl_proxy_marshal_constructor_versioned(reg, 0, &wl_shell_interface, 1, name, "wl_shell", 1, NULL);
	else if (!strcmp(iface, "wl_scaler"))
		scaler = wl_proxy_marshal_constructor_versioned(reg, 0, &wl_scaler_iface, 2, name, "wl_scaler", 2, NULL);
}
static void reg_remove(void *d, struct wl_proxy *reg, uint32_t name) { (void)d, (void)reg, (void)name; }
static const struct {
	void (*global)(void *, struct wl_proxy *, uint32_t, const char *, uint32_t);
	void (*remove)(void *, struct wl_proxy *, uint32_t);
} reg_listener = {reg_global, reg_remove};

static void ss_ping(void *d, struct wl_proxy *ss, uint32_t serial) {
	(void)d;
	wl_proxy_marshal(ss, 0, serial); /* pong */
}
static void ss_configure(void *d, struct wl_proxy *ss, uint32_t e, int32_t w, int32_t h) { (void)d, (void)ss, (void)e, (void)w, (void)h; }
static void ss_popup(void *d, struct wl_proxy *ss) { (void)d, (void)ss; }
static const struct {
	void (*ping)(void *, struct wl_proxy *, uint32_t);
	void (*configure)(void *, struct wl_proxy *, uint32_t, int32_t, int32_t);
	void (*popup_done)(void *, struct wl_proxy *);
} ss_listener = {ss_ping, ss_configure, ss_popup};

EGLDisplay plat_init(void) {
	if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/run", 0); /* old libwayland: name is relative */
	display = wl_display_connect("wayland-0");
	if (!display) {
		fprintf(stderr, "mgl-server(tv): cannot connect to wayland-0\n");
		return EGL_NO_DISPLAY;
	}
	struct wl_proxy *reg = wl_proxy_marshal_constructor((struct wl_proxy *)display, 1, &wl_registry_interface, NULL);
	wl_proxy_add_listener(reg, (void (**)(void))&reg_listener, NULL);
	wl_display_roundtrip(display);
	const char *s = getenv("MGL_OUTPUT_SIZE");
	if (s) sscanf(s, "%dx%d", &out_w, &out_h);
	EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)display);
	EGLint ma, mi;
	if (!compositor || !shell || dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &ma, &mi)) {
		fprintf(stderr, "mgl-server(tv): wayland/EGL init failed\n");
		return EGL_NO_DISPLAY;
	}
	return dpy;
}

EGLSurface plat_create_window_surface(EGLDisplay dpy, EGLConfig cfg, int w, int h, void **out) {
	struct win *win = calloc(1, sizeof *win);
	win->surface = wl_proxy_marshal_constructor(compositor, 0, &wl_surface_interface, NULL);
	win->shell_surface = wl_proxy_marshal_constructor(shell, 0, &wl_shell_surface_interface, NULL, win->surface);
	wl_proxy_add_listener(win->shell_surface, (void (**)(void))&ss_listener, NULL);
	wl_proxy_marshal(win->shell_surface, 3); /* set_toplevel */
	wl_proxy_marshal(win->shell_surface, 8, "Chromium"); /* set_title */
	if (scaler && out_w > 0 && (out_w != w || out_h != h)) {
		win->viewport = wl_proxy_marshal_constructor(scaler, 1, &wl_viewport_iface, NULL, win->surface);
		wl_proxy_marshal(win->viewport, 3, out_w, out_h); /* set_destination */
	}
	win->egl = wl_egl_window_create((struct wl_surface *)win->surface, w, h);
	EGLSurface s = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)win->egl, NULL);
	*out = win;
	wl_display_flush(display);
	return s;
}
void plat_resize_window(void *p, int w, int h) {
	struct win *win = p;
	wl_egl_window_resize(win->egl, w, h, 0, 0);
}
void plat_destroy_window(void *p) {
	struct win *win = p;
	if (win->egl) wl_egl_window_destroy(win->egl);
	if (win->viewport) wl_proxy_marshal(win->viewport, 0), wl_proxy_destroy(win->viewport);
	wl_proxy_destroy(win->shell_surface);
	wl_proxy_marshal(win->surface, 0); /* wl_surface.destroy */
	wl_proxy_destroy(win->surface);
	free(win);
	wl_display_flush(display);
}

/* The TV blends the graphics plane over the video plane by alpha: make the
 * finished frame fully opaque so nothing translucent lets video through. */
void plat_before_swap(struct ssurface *s) {
	(void)s;
	if (getenv("MGL_KEEP_ALPHA")) return;
	GLint fbo = 0;
	GLboolean mask[4], scissor = glIsEnabled(GL_SCISSOR_TEST);
	GLfloat cc[4];
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
	glGetBooleanv(GL_COLOR_WRITEMASK, mask);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
	if (fbo) glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (scissor) glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glColorMask(mask[0], mask[1], mask[2], mask[3]);
	glClearColor(cc[0], cc[1], cc[2], cc[3]);
	if (scissor) glEnable(GL_SCISSOR_TEST);
	if (fbo) glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
}
void plat_after_swap(struct ssurface *s) {
	(void)s;
	wl_display_dispatch_pending(display); /* answer pings etc. */
	wl_display_flush(display);
}
