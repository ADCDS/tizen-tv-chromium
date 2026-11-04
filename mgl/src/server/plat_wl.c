/* mgl-server platform for the TV: each EGL window surface is a wl_shell window
 * on the Tizen compositor (E20), rendered by the Mali through wayland-egl, so
 * frames reach the screen with no CPU copy. The TV's libwayland-client is
 * ~1.10, so requests use wl_proxy_marshal(_constructor) directly.
 * Env: MGL_OUTPUT_SIZE=WxH scales windows to that size via wl_scaler.
 *
 * Window control (MGL_WINDOW_SOCKET=path, used by the kiosk in GPU mode): our
 * window is the one on screen, so it also owns the remote. MGL_GRAB_KEYS (TV
 * keymap codes) are grabbed with tizen_keyrouter in "topmost" mode: they come
 * to us only while our window is on top and go back to the TV as soon as its
 * own UI (Source, Home, another app) covers it. Clients of the socket get lines
 *   key <evdev code> <0|1>   focus <0|1>   visible <0|1>
 * and may send "show" or "hide". The Tizen protocols come from the TV's
 * libtizen-extension-client at run time; requests are looked up by name. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-egl-core.h>

#include "server.h"

extern const struct wl_interface wl_registry_interface, wl_compositor_interface, wl_surface_interface,
    wl_shell_interface, wl_shell_surface_interface, wl_seat_interface, wl_keyboard_interface;

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
	struct wl_proxy *surface, *shell_surface, *viewport, *visibility;
	struct wl_egl_window *egl;
	int visible; /* not fully covered (tizen_visibility); swaps are skipped when 0 */
	int shown;   /* has presented a frame */
};

/* Window control state (see the top of the file). */
static const struct wl_interface *policy_if, *visibility_if, *keyrouter_if; /* from libtizen-extension-client */
static struct wl_proxy *seat, *keyboard, *policy, *keyrouter;
static uint32_t grab_keys[64];
static int ngrab, wsock = -1, focus_state, visible_state; /* visible_state: a window of ours is on screen */
#define MAXWIN 8
#define MAXWC 8
static struct win *wins[MAXWIN];
static int nwins;
static struct {
	int fd, len;
	char buf[128];
} wc[MAXWC];

static int req(const struct wl_interface *i, const char *name) { /* request opcode by name, -1 if absent */
	for (int k = 0; i && k < i->method_count; k++)
		if (!strcmp(i->methods[k].name, name)) return k;
	return -1;
}
static void wc_send(int k, const char *line) {
	if (wc[k].fd >= 0 && write(wc[k].fd, line, strlen(line)) < 0 && errno != EAGAIN) close(wc[k].fd), wc[k].fd = -1;
}
static void broadcast(const char *line) {
	for (int k = 0; k < MAXWC; k++) wc_send(k, line);
}
static void set_focus(int f) {
	if (f == focus_state) return;
	focus_state = f;
	broadcast(f ? "focus 1\n" : "focus 0\n");
}
static void update_visible(void) {
	int v = 0;
	for (int i = 0; i < nwins; i++) v |= wins[i]->visible && wins[i]->shown;
	if (v == visible_state) return;
	visible_state = v;
	broadcast(v ? "visible 1\n" : "visible 0\n");
	if (verbose) fprintf(stderr, "mgl: window %s\n", v ? "visible" : "hidden");
}

static void kb_keymap(void *d, struct wl_proxy *kb, uint32_t fmt, int32_t fd, uint32_t size) {
	(void)d, (void)kb, (void)fmt, (void)size;
	close(fd);
}
static void kb_enter(void *d, struct wl_proxy *kb, uint32_t serial, struct wl_proxy *surf, struct wl_array *keys) {
	(void)d, (void)kb, (void)serial, (void)keys;
	for (int i = 0; i < nwins; i++)
		if (wins[i]->surface == surf) set_focus(1);
}
static void kb_leave(void *d, struct wl_proxy *kb, uint32_t serial, struct wl_proxy *surf) {
	(void)d, (void)kb, (void)serial, (void)surf;
	set_focus(0);
}
static void kb_key(void *d, struct wl_proxy *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	(void)d, (void)kb, (void)serial, (void)time;
	char line[48];
	snprintf(line, sizeof line, "key %u %u\n", key, state);
	broadcast(line);
}
static void kb_mods(void *d, struct wl_proxy *kb, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {
	(void)d, (void)kb, (void)serial, (void)dep, (void)lat, (void)lock, (void)grp;
}
static void kb_repeat(void *d, struct wl_proxy *kb, int32_t rate, int32_t delay) { (void)d, (void)kb, (void)rate, (void)delay; }
static const struct {
	void (*keymap)(void *, struct wl_proxy *, uint32_t, int32_t, uint32_t);
	void (*enter)(void *, struct wl_proxy *, uint32_t, struct wl_proxy *, struct wl_array *);
	void (*leave)(void *, struct wl_proxy *, uint32_t, struct wl_proxy *);
	void (*key)(void *, struct wl_proxy *, uint32_t, uint32_t, uint32_t, uint32_t);
	void (*modifiers)(void *, struct wl_proxy *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
	void (*repeat_info)(void *, struct wl_proxy *, int32_t, int32_t);
} kb_listener = {kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat};

/* tizen_keyrouter v1 events: only keygrab_notify matters; the rest are no-ops. */
static void kr_grab(void *d, struct wl_proxy *kr, struct wl_proxy *surf, uint32_t key, uint32_t mode, uint32_t err) {
	(void)d, (void)kr, (void)surf, (void)mode;
	if (err && verbose) fprintf(stderr, "mgl: keygrab %u failed: error %u\n", key, err);
}
static void kr_list(void *d, struct wl_proxy *kr, struct wl_proxy *surf, struct wl_array *a) { (void)d, (void)kr, (void)surf, (void)a; }
static void kr_ou(void *d, struct wl_proxy *kr, struct wl_proxy *surf, uint32_t v) { (void)d, (void)kr, (void)surf, (void)v; }
static void kr_u(void *d, struct wl_proxy *kr, uint32_t v) { (void)d, (void)kr, (void)v; }
static const struct {
	void (*keygrab_notify)(void *, struct wl_proxy *, struct wl_proxy *, uint32_t, uint32_t, uint32_t);
	void (*keygrab_notify_list)(void *, struct wl_proxy *, struct wl_proxy *, struct wl_array *);
	void (*getgrab_notify_list)(void *, struct wl_proxy *, struct wl_proxy *, struct wl_array *);
	void (*set_register_none_key_notify)(void *, struct wl_proxy *, struct wl_proxy *, uint32_t);
	void (*keyregister_notify)(void *, struct wl_proxy *, uint32_t);
	void (*set_input_config_notify)(void *, struct wl_proxy *, uint32_t);
	void (*key_cancel)(void *, struct wl_proxy *, uint32_t);
} kr_listener = {kr_grab, kr_list, kr_list, kr_ou, kr_u, kr_u, kr_u};

/* tizen_visibility.notify: 0 unobscured, 1 partially obscured, 2 fully obscured. */
static void vis_notify(void *d, struct wl_proxy *v, uint32_t vis) {
	(void)v;
	((struct win *)d)->visible = vis < 2;
	update_visible();
}
static const struct {
	void (*notify)(void *, struct wl_proxy *, uint32_t);
} vis_listener = {vis_notify};

/* Topmost grabs on our window. An iconified window still counts as topmost for
 * tizen_keyrouter (it stays above the TV's own windows in the stack), so the
 * grabs are dropped while hidden or the TV would lose the remote. */
static void grab(struct win *win, int on) {
	for (int k = 0; keyrouter && k < ngrab; k++) {
		if (on) wl_proxy_marshal(keyrouter, req(keyrouter_if, "set_keygrab"), win->surface, grab_keys[k], 2 /* topmost */);
		else wl_proxy_marshal(keyrouter, req(keyrouter_if, "unset_keygrab"), win->surface, grab_keys[k]);
	}
}

static void window_command(const char *c) {
	int hide = !strcmp(c, "hide"), show = !strcmp(c, "show");
	if (verbose) fprintf(stderr, "mgl: window command \"%s\"\n", c);
	for (int i = 0; policy && i < nwins; i++) {
		if (hide) {
			grab(wins[i], 0);
			wl_proxy_marshal(policy, req(policy_if, "iconify"), wins[i]->surface);
		}
		if (show) {
			grab(wins[i], 1);
			wl_proxy_marshal(policy, req(policy_if, "uniconify"), wins[i]->surface);
			wl_proxy_marshal(policy, req(policy_if, "activate"), wins[i]->surface);
		}
	}
	wl_display_flush(display);
}

static void window_control_init(const char *path) {
	const char *keys = getenv("MGL_GRAB_KEYS");
	for (const char *p = keys; p && *p && ngrab < 64;) {
		grab_keys[ngrab++] = (uint32_t)strtoul(p, (char **)&p, 10);
		while (*p == ',' || *p == ' ') p++;
	}
	if (seat) {
		keyboard = wl_proxy_marshal_constructor(seat, 1 /* get_keyboard */, &wl_keyboard_interface, NULL);
		wl_proxy_add_listener(keyboard, (void (**)(void))&kb_listener, NULL);
	}
	if (keyrouter) wl_proxy_add_listener(keyrouter, (void (**)(void))&kr_listener, NULL);
	for (int k = 0; k < MAXWC; k++) wc[k].fd = -1;
	wsock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	strncpy(a.sun_path, path, sizeof a.sun_path - 1);
	unlink(path);
	if (bind(wsock, (struct sockaddr *)&a, sizeof a) || listen(wsock, 4)) {
		perror("mgl-server: window socket");
		close(wsock), wsock = -1;
		return;
	}
	chmod(path, 0777);
	fprintf(stderr, "mgl-server(tv): window control on %s: %d keys, seat %s, keyrouter %s, policy %s\n", path, ngrab,
	        seat ? "yes" : "no", keyrouter ? "yes" : "no", policy ? "yes" : "no");
}

static void reg_global(void *d, struct wl_proxy *reg, uint32_t name, const char *iface, uint32_t ver) {
	(void)d;
	if (!strcmp(iface, "wl_compositor"))
		compositor = wl_proxy_marshal_constructor_versioned(reg, 0, &wl_compositor_interface, 3, name, "wl_compositor", 3, NULL);
	else if (!strcmp(iface, "wl_shell"))
		shell = wl_proxy_marshal_constructor_versioned(reg, 0, &wl_shell_interface, 1, name, "wl_shell", 1, NULL);
	else if (!strcmp(iface, "wl_scaler"))
		scaler = wl_proxy_marshal_constructor_versioned(reg, 0, &wl_scaler_iface, 2, name, "wl_scaler", 2, NULL);
	else if (!strcmp(iface, "wl_seat") && !seat)
		seat = wl_proxy_marshal_constructor_versioned(reg, 0, &wl_seat_interface, 1, name, "wl_seat", 1, NULL);
	else if (!strcmp(iface, "tizen_keyrouter") && keyrouter_if)
		keyrouter = wl_proxy_marshal_constructor_versioned(reg, 0, keyrouter_if, 1, name, "tizen_keyrouter", 1, NULL);
	else if (!strcmp(iface, "tizen_policy") && policy_if) {
		/* No listener: events we don't ask for are dropped by libwayland. */
		uint32_t v = ver < (uint32_t)policy_if->version ? ver : (uint32_t)policy_if->version;
		policy = wl_proxy_marshal_constructor_versioned(reg, 0, policy_if, v, name, "tizen_policy", v, NULL);
	}
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
	const char *wpath = getenv("MGL_WINDOW_SOCKET");
	if (wpath) {
		void *ext = dlopen("libtizen-extension-client.so.0", RTLD_NOW | RTLD_GLOBAL);
		if (ext) {
			policy_if = dlsym(ext, "tizen_policy_interface");
			visibility_if = dlsym(ext, "tizen_visibility_interface");
			keyrouter_if = dlsym(ext, "tizen_keyrouter_interface");
		} else
			fprintf(stderr, "mgl-server(tv): %s\n", dlerror());
	}
	struct wl_proxy *reg = wl_proxy_marshal_constructor((struct wl_proxy *)display, 1, &wl_registry_interface, NULL);
	wl_proxy_add_listener(reg, (void (**)(void))&reg_listener, NULL);
	wl_display_roundtrip(display);
	if (wpath) window_control_init(wpath);
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
	win->visible = 1;
	if (wsock >= 0 && nwins < MAXWIN) {
		grab(win, 1);
		if (policy && visibility_if) {
			win->visibility = wl_proxy_marshal_constructor(policy, req(policy_if, "get_visibility"), visibility_if, NULL, win->surface);
			wl_proxy_add_listener(win->visibility, (void (**)(void))&vis_listener, win);
		}
		wins[nwins++] = win;
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
	for (int i = 0; i < nwins; i++)
		if (wins[i] == win) wins[i] = wins[--nwins];
	if (win->visibility) wl_proxy_marshal(win->visibility, 0), wl_proxy_destroy(win->visibility); /* destroy */
	update_visible();
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
	struct win *win = s->win;
	if (win && !win->shown) win->shown = 1, update_visible();
	wl_display_dispatch_pending(display); /* answer pings etc. */
	wl_display_flush(display);
}

/* A hidden or covered window gets no frame callbacks, and a swap would then
 * block the whole server (including "show"): skip swaps while not visible. */
int plat_window_visible(void *p) { return ((struct win *)p)->visible; }

/* Event loop hooks: the Wayland connection (keys and visibility arrive while
 * the page is idle, with no swaps) and the window socket with its clients. */
static int pf_client[MAXWC];
int plat_poll_fds(struct pollfd *pf, int max) {
	int n = 0;
	while (wl_display_prepare_read(display) != 0) wl_display_dispatch_pending(display);
	wl_display_flush(display);
	pf[n++] = (struct pollfd){wl_display_get_fd(display), POLLIN, 0};
	if (wsock >= 0 && n < max) pf[n++] = (struct pollfd){wsock, POLLIN, 0};
	for (int k = 0; wsock >= 0 && k < MAXWC; k++) {
		pf_client[k] = -1;
		if (wc[k].fd >= 0 && n < max) pf_client[k] = n, pf[n++] = (struct pollfd){wc[k].fd, POLLIN, 0};
	}
	return n;
}
void plat_poll_ready(struct pollfd *pf, int n) {
	if (n < 1) return;
	if (pf[0].revents & POLLIN) wl_display_read_events(display);
	else wl_display_cancel_read(display);
	wl_display_dispatch_pending(display);
	if (wsock >= 0 && n > 1 && (pf[1].revents & POLLIN)) {
		int fd = accept4(wsock, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
		for (int k = 0; fd >= 0 && k < MAXWC; k++)
			if (wc[k].fd < 0) {
				wc[k].fd = fd, wc[k].len = 0, fd = -1;
				wc_send(k, focus_state ? "focus 1\n" : "focus 0\n");
				wc_send(k, visible_state ? "visible 1\n" : "visible 0\n");
			}
		if (fd >= 0) close(fd); /* full */
	}
	for (int k = 0; wsock >= 0 && k < MAXWC; k++) {
		int i = pf_client[k];
		if (i < 0 || i >= n || !(pf[i].revents & (POLLIN | POLLHUP | POLLERR)) || wc[k].fd < 0) continue;
		ssize_t got = read(wc[k].fd, wc[k].buf + wc[k].len, sizeof wc[k].buf - 1 - (size_t)wc[k].len);
		if (got <= 0) {
			if (got < 0 && errno == EAGAIN) continue;
			close(wc[k].fd), wc[k].fd = -1;
			continue;
		}
		wc[k].len += (int)got;
		wc[k].buf[wc[k].len] = 0;
		char *line = wc[k].buf, *nl;
		while ((nl = strchr(line, '\n'))) {
			*nl = 0;
			window_command(line);
			line = nl + 1;
		}
		wc[k].len = (int)strlen(line);
		memmove(wc[k].buf, line, (size_t)wc[k].len + 1);
		if (wc[k].len >= (int)sizeof wc[k].buf - 1) wc[k].len = 0; /* overlong line: drop */
	}
	wl_display_flush(display);
}
