/* mgl client: EGL 1.4 + EGL_KHR_fence_sync, as ANGLE's GLES-EGL backend uses it
 * (traced on Chromium 150): eglGetDisplay, eglInitialize, config scan via
 * eglGetConfigAttrib (answered from a local cache), contexts (ES2 only),
 * one X11 window surface, pbuffers, fences, swaps. Handles are client ids. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

#define EXPORT __attribute__((visibility("default")))
#define DPY ((EGLDisplay)(uintptr_t)1)

struct cfg {
	uint32_t id;
	EGLint a[64];
};
static struct cfg *cfgs;
static int ncfgs;
static int initialized;
static __thread EGLint last_error = EGL_SUCCESS;
static EGLenum bound_api = EGL_OPENGL_ES_API;

static const EGLenum cfg_attr_names[] = {
#define X(a) a,
	MGL_CFGATTRS
#undef X
};
#define NCFGATTR (sizeof cfg_attr_names / sizeof cfg_attr_names[0])

static EGLBoolean fail(EGLint e) {
	last_error = e;
	return EGL_FALSE;
}
static EGLBoolean ok(void) {
	last_error = EGL_SUCCESS;
	return EGL_TRUE;
}

EXPORT EGLDisplay eglGetDisplay(EGLNativeDisplayType d) {
	(void)d;
	return DPY;
}
EXPORT EGLDisplay eglGetPlatformDisplayEXT(EGLenum p, void *d, const EGLint *a) {
	(void)p, (void)d, (void)a;
	return DPY;
}
EXPORT EGLDisplay eglGetPlatformDisplay(EGLenum p, void *d, const EGLAttrib *a) {
	(void)p, (void)d, (void)a;
	return DPY;
}

static void load_configs(void) {
	if (cfgs) return;
	mgl_lock();
	mgl_begin(MGL_OP_EGL_GET_CONFIGS, 1);
	struct mgl_reader r = mgl_end_sync();
	ncfgs = (int)mgl_u32(&r);
	cfgs = calloc((size_t)ncfgs + 1, sizeof *cfgs);
	for (int i = 0; i < ncfgs; i++) {
		cfgs[i].id = mgl_u32(&r);
		for (size_t k = 0; k < NCFGATTR; k++) cfgs[i].a[k] = mgl_i32(&r);
	}
	mgl_unlock();
}
static struct cfg *cfg_get(EGLConfig c) {
	uint32_t id = (uint32_t)(uintptr_t)c;
	for (int i = 0; i < ncfgs; i++)
		if (cfgs[i].id == id) return &cfgs[i];
	return NULL;
}

EXPORT EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
	if (dpy != DPY) return fail(EGL_BAD_DISPLAY);
	if (!initialized) {
		mgl_lock();
		mgl_begin(MGL_OP_EGL_INITIALIZE, 1);
		struct mgl_reader r = mgl_end_sync();
		int good = (int)mgl_u32(&r);
		mgl_unlock();
		if (!good) return fail(EGL_NOT_INITIALIZED);
		load_configs();
		initialized = 1;
	}
	if (major) *major = 1;
	if (minor) *minor = 4;
	return ok();
}
EXPORT EGLBoolean eglTerminate(EGLDisplay dpy) {
	(void)dpy;
	return ok();
}

EXPORT const char *eglQueryString(EGLDisplay dpy, EGLint name) {
	if (dpy == EGL_NO_DISPLAY) return name == EGL_EXTENSIONS ? "" : NULL;
	switch (name) {
	case EGL_VENDOR: return "mgl (Mali-450 relay)";
	case EGL_VERSION: return "1.4 mgl";
	case EGL_CLIENT_APIS: return "OpenGL_ES";
	case EGL_EXTENSIONS: return "EGL_KHR_fence_sync";
	}
	last_error = EGL_BAD_PARAMETER;
	return NULL;
}

EXPORT EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig *out, EGLint size, EGLint *num) {
	(void)dpy;
	load_configs();
	if (!num) return fail(EGL_BAD_PARAMETER);
	if (!out) {
		*num = ncfgs;
		return ok();
	}
	EGLint n = ncfgs < size ? ncfgs : size;
	for (EGLint i = 0; i < n; i++) out[i] = (EGLConfig)(uintptr_t)cfgs[i].id;
	*num = n;
	return ok();
}
EXPORT EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrs, EGLConfig *out, EGLint size, EGLint *num) {
	(void)dpy;
	load_configs();
	if (!num) return fail(EGL_BAD_PARAMETER);
	uint32_t n = 0;
	while (attrs && attrs[n] != EGL_NONE) n += 2;
	/* EGL_CONFIG_ID selects one config and overrides everything else. Our ids
	 * are the relay's, not the driver's, so answer locally. */
	for (uint32_t i = 0; i < n; i += 2)
		if (attrs[i] == EGL_CONFIG_ID) {
			struct cfg *cf = cfg_get((EGLConfig)(uintptr_t)attrs[i + 1]);
			*num = cf ? 1 : 0;
			if (cf && out && size > 0) out[0] = (EGLConfig)(uintptr_t)cf->id;
			return ok();
		}
	mgl_lock();
	mgl_begin(MGL_OP_EGL_CHOOSE_CONFIG, 1);
	EGLint none = EGL_NONE;
	if (n) {
		EGLint *copy = malloc((n + 1) * sizeof *copy);
		memcpy(copy, attrs, n * sizeof *copy);
		copy[n] = EGL_NONE;
		mgl_wblob(copy, (n + 1) * sizeof *copy);
		free(copy);
	} else
		mgl_wblob(&none, sizeof none);
	struct mgl_reader r = mgl_end_sync();
	EGLint total = (EGLint)mgl_u32(&r), k = 0;
	for (EGLint i = 0; i < total; i++) {
		uint32_t id = mgl_u32(&r);
		if (out && k < size) out[k++] = (EGLConfig)(uintptr_t)id;
	}
	mgl_unlock();
	*num = out ? k : total;
	return ok();
}
static int x_root_visual(void);
EXPORT EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig c, EGLint attr, EGLint *value) {
	(void)dpy;
	struct cfg *cf = cfg_get(c);
	if (!cf || !value) return fail(EGL_BAD_CONFIG);
	if (attr == EGL_CONFIG_ID) {
		*value = (EGLint)cf->id;
		return ok();
	}
	if (attr == EGL_NATIVE_VISUAL_ID) {
		/* Chromium on X11 wants the config whose visual is the window's (the
		 * X root visual, no compositing WM); the Mali reports DRM fourccs. */
		EGLint surf = 0;
		for (size_t k = 0; k < NCFGATTR; k++)
			if (cfg_attr_names[k] == EGL_SURFACE_TYPE) surf = cf->a[k];
		*value = (surf & EGL_WINDOW_BIT) ? x_root_visual() : 0;
		return ok();
	}
	for (size_t k = 0; k < NCFGATTR; k++)
		if ((EGLint)cfg_attr_names[k] == attr) {
			*value = cf->a[k];
			return ok();
		}
	if (attr == 0x3339 /* EGL_COLOR_COMPONENT_TYPE_EXT */) {
		*value = 0x333A; /* EGL_COLOR_COMPONENT_TYPE_FIXED_EXT: the Mali has no float configs */
		return ok();
	}
	*value = 0; /* attributes we don't ship (e.g. transparent values) read as 0 */
	return ok();
}

/* ---- X11 (via libxcb, loaded on demand) ---- */
static void *xlib;
static void *xconn;
static void *(*x_connect)(const char *, int *);
static unsigned (*get_geom)(void *, unsigned);
static void *(*get_geom_reply)(void *, unsigned, void *);
static const void *(*get_setup)(void *);
static int x_open(void) {
	if (!xlib) {
		xlib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
		if (!xlib) return 0;
		x_connect = (void *(*)(const char *, int *))dlsym(xlib, "xcb_connect");
		get_geom = (unsigned (*)(void *, unsigned))dlsym(xlib, "xcb_get_geometry");
		get_geom_reply = (void *(*)(void *, unsigned, void *))dlsym(xlib, "xcb_get_geometry_reply");
		get_setup = (const void *(*)(void *))dlsym(xlib, "xcb_get_setup");
	}
	if (!x_connect || !get_geom || !get_geom_reply || !get_setup) return 0;
	if (!xconn) xconn = x_connect(NULL, NULL);
	return xconn != NULL;
}
static int x_root_visual(void) {
	static int visual = -1;
	if (visual >= 0) return visual;
	visual = 0;
	if (!x_open()) return 0;
	/* xcb_setup_t is followed by the vendor string (padded) and pixmap formats,
	 * then the first xcb_screen_t, whose root_visual is at offset 32. */
	const uint8_t *setup = get_setup(xconn);
	uint16_t vendor_len;
	uint8_t nformats;
	memcpy(&vendor_len, setup + 24, 2);
	nformats = setup[29];
	const uint8_t *screen = setup + 40 + ((vendor_len + 3) & ~3) + 8 * nformats;
	uint32_t v;
	memcpy(&v, screen + 32, 4);
	visual = (int)v;
	return visual;
}

/* ---- surfaces ---- */
static int x_window_size(unsigned long win, int *w, int *h) {
	if (!x_open()) return 0;
	void *conn = xconn;
	/* xcb_get_geometry_reply_t: u8 type, u8 depth, u16 seq, u32 len, u32 root, i16 x, i16 y, u16 w, u16 h */
	uint8_t *rep = get_geom_reply(conn, get_geom(conn, (unsigned)win), NULL);
	if (!rep) return 0;
	uint16_t ww, hh;
	memcpy(&ww, rep + 16, 2);
	memcpy(&hh, rep + 18, 2);
	free(rep);
	*w = ww, *h = hh;
	return 1;
}

struct csurf {
	unsigned long xwin;
	int w, h, swaps;
};
static struct csurf *csurfs[4096];

static EGLSurface create_window(EGLConfig c, unsigned long win) {
	if (!cfg_get(c)) return (fail(EGL_BAD_CONFIG), EGL_NO_SURFACE);
	int w = 0, h = 0;
	if (!x_window_size(win, &w, &h)) {
		const char *s = getenv("MGL_WINDOW_SIZE");
		if (!s || sscanf(s, "%dx%d", &w, &h) != 2) w = 1280, h = 720;
	}
	uint32_t id = mgl_new_surface();
	mgl_lock();
	mgl_begin(MGL_OP_EGL_CREATE_WINDOW, 1);
	mgl_w32(id), mgl_w32((uint32_t)(uintptr_t)c), mgl_w32((uint32_t)w), mgl_w32((uint32_t)h);
	struct mgl_reader r = mgl_end_sync();
	int good = (int)mgl_u32(&r);
	mgl_unlock();
	if (!good || id >= 4096) return (fail(EGL_BAD_ALLOC), EGL_NO_SURFACE);
	struct csurf *s = calloc(1, sizeof *s);
	s->xwin = win, s->w = w, s->h = h;
	csurfs[id] = s;
	ok();
	return (EGLSurface)(uintptr_t)id;
}
EXPORT EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig c, EGLNativeWindowType win, const EGLint *a) {
	(void)dpy, (void)a;
	return create_window(c, (unsigned long)(uintptr_t)win);
}
EXPORT EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig c, void *win, const EGLint *a) {
	(void)dpy, (void)a;
	return create_window(c, win ? *(unsigned long *)win : 0);
}
EXPORT EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig c, const EGLint *a) {
	(void)dpy;
	int w = 1, h = 1;
	for (int i = 0; a && a[i] != EGL_NONE; i += 2) {
		if (a[i] == EGL_WIDTH) w = a[i + 1];
		if (a[i] == EGL_HEIGHT) h = a[i + 1];
	}
	uint32_t id = mgl_new_surface();
	mgl_lock();
	mgl_begin(MGL_OP_EGL_CREATE_PBUFFER, 1);
	mgl_w32(id), mgl_w32((uint32_t)(uintptr_t)c), mgl_w32((uint32_t)w), mgl_w32((uint32_t)h);
	struct mgl_reader r = mgl_end_sync();
	int good = (int)mgl_u32(&r);
	mgl_unlock();
	if (!good) return (fail(EGL_BAD_ALLOC), EGL_NO_SURFACE);
	ok();
	return (EGLSurface)(uintptr_t)id;
}
EXPORT EGLSurface eglCreatePixmapSurface(EGLDisplay d, EGLConfig c, EGLNativePixmapType p, const EGLint *a) {
	(void)d, (void)c, (void)p, (void)a;
	return (fail(EGL_BAD_MATCH), EGL_NO_SURFACE);
}
EXPORT EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay d, EGLenum t, EGLClientBuffer b, EGLConfig c, const EGLint *a) {
	(void)d, (void)t, (void)b, (void)c, (void)a;
	return (fail(EGL_BAD_MATCH), EGL_NO_SURFACE);
}
EXPORT EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface s) {
	(void)dpy;
	uint32_t id = (uint32_t)(uintptr_t)s;
	mgl_lock();
	mgl_begin(MGL_OP_EGL_DESTROY_SURFACE, 0);
	mgl_w32(id);
	mgl_end();
	mgl_unlock();
	if (id < 4096) free(csurfs[id]), csurfs[id] = NULL;
	return ok();
}
EXPORT EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface s, EGLint attr, EGLint *value) {
	(void)dpy;
	mgl_lock();
	mgl_begin(MGL_OP_EGL_QUERY_SURFACE, 1);
	mgl_w32((uint32_t)(uintptr_t)s), mgl_w32((uint32_t)attr);
	struct mgl_reader r = mgl_end_sync();
	int good = (int)mgl_u32(&r);
	EGLint v = mgl_i32(&r);
	mgl_unlock();
	if (!good) return fail(EGL_BAD_ATTRIBUTE);
	if (value) *value = v;
	return ok();
}
EXPORT EGLBoolean eglSurfaceAttrib(EGLDisplay d, EGLSurface s, EGLint a, EGLint v) {
	(void)d, (void)s, (void)a, (void)v;
	return ok();
}
EXPORT EGLBoolean eglBindTexImage(EGLDisplay d, EGLSurface s, EGLint b) {
	(void)d, (void)s, (void)b;
	return fail(EGL_BAD_MATCH);
}
EXPORT EGLBoolean eglReleaseTexImage(EGLDisplay d, EGLSurface s, EGLint b) {
	(void)d, (void)s, (void)b;
	return fail(EGL_BAD_MATCH);
}
EXPORT EGLBoolean eglCopyBuffers(EGLDisplay d, EGLSurface s, EGLNativePixmapType p) {
	(void)d, (void)s, (void)p;
	return fail(EGL_BAD_MATCH);
}

EXPORT EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface s) {
	(void)dpy;
	uint32_t id = (uint32_t)(uintptr_t)s;
	struct csurf *cs = id < 4096 ? csurfs[id] : NULL;
	mgl_lock();
	if (cs && ++cs->swaps % 30 == 0) { /* follow window resizes */
		int w, h;
		if (x_window_size(cs->xwin, &w, &h) && (w != cs->w || h != cs->h)) {
			cs->w = w, cs->h = h;
			mgl_begin(MGL_OP_EGL_RESIZE_WINDOW, 0);
			mgl_w32(id), mgl_w32((uint32_t)w), mgl_w32((uint32_t)h);
			mgl_end();
		}
	}
	mgl_begin(MGL_OP_EGL_SWAP, 1);
	mgl_w32(id);
	struct mgl_reader r = mgl_end_sync();
	int good = (int)mgl_u32(&r);
	mgl_unlock();
	return good ? ok() : fail(EGL_BAD_SURFACE);
}
EXPORT EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
	(void)dpy;
	mgl_lock();
	mgl_begin(MGL_OP_EGL_SWAP_INTERVAL, 0);
	mgl_w32((uint32_t)interval);
	mgl_end();
	mgl_unlock();
	return ok();
}

/* ---- contexts ---- */
EXPORT EGLBoolean eglBindAPI(EGLenum api) {
	if (api != EGL_OPENGL_ES_API) return fail(EGL_BAD_PARAMETER);
	bound_api = api;
	return ok();
}
EXPORT EGLenum eglQueryAPI(void) { return bound_api; }

EXPORT EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig c, EGLContext share, const EGLint *a) {
	(void)dpy;
	EGLint major = 1;
	for (int i = 0; a && a[i] != EGL_NONE; i += 2)
		if (a[i] == EGL_CONTEXT_CLIENT_VERSION) major = a[i + 1];
	if (major > 2) return (fail(EGL_BAD_MATCH), EGL_NO_CONTEXT); /* the Mali is ES 2.0 only */
	if (!cfg_get(c)) return (fail(EGL_BAD_CONFIG), EGL_NO_CONTEXT);
	mgl_lock();
	uint32_t id = mgl_new_ctx((uint32_t)(uintptr_t)share);
	mgl_begin(MGL_OP_EGL_CREATE_CONTEXT, 1);
	mgl_w32(id), mgl_w32((uint32_t)(uintptr_t)c), mgl_w32((uint32_t)(uintptr_t)share), mgl_w32(2);
	struct mgl_reader r = mgl_end_sync();
	int good = (int)mgl_u32(&r);
	mgl_unlock();
	if (!good || !id) return (fail(EGL_BAD_ALLOC), EGL_NO_CONTEXT);
	ok();
	return (EGLContext)(uintptr_t)id;
}
EXPORT EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
	(void)dpy;
	mgl_lock();
	mgl_begin(MGL_OP_EGL_DESTROY_CONTEXT, 0);
	mgl_w32((uint32_t)(uintptr_t)ctx);
	mgl_end();
	mgl_unlock();
	return ok();
}
EXPORT EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
	(void)dpy;
	mgl_set_current((uint32_t)(uintptr_t)ctx, (uint32_t)(uintptr_t)draw, (uint32_t)(uintptr_t)read);
	return ok(); /* sent lazily before the next GL call from this thread */
}
EXPORT EGLContext eglGetCurrentContext(void) { return (EGLContext)(uintptr_t)mgl_current()->ctx; }
EXPORT EGLSurface eglGetCurrentSurface(EGLint which) {
	return (EGLSurface)(uintptr_t)(which == EGL_READ ? mgl_current()->read : mgl_current()->draw);
}
EXPORT EGLDisplay eglGetCurrentDisplay(void) { return mgl_current()->ctx ? DPY : EGL_NO_DISPLAY; }
EXPORT EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attr, EGLint *value) {
	(void)dpy;
	if (!value) return fail(EGL_BAD_PARAMETER);
	switch (attr) {
	case EGL_CONTEXT_CLIENT_VERSION: *value = 2; break;
	case EGL_CONTEXT_CLIENT_TYPE: *value = EGL_OPENGL_ES_API; break;
	case EGL_RENDER_BUFFER: *value = EGL_BACK_BUFFER; break;
	case EGL_CONFIG_ID: *value = ncfgs ? (EGLint)cfgs[0].id : 0; break;
	default: return fail(EGL_BAD_ATTRIBUTE);
	}
	(void)ctx;
	return ok();
}
EXPORT EGLBoolean eglReleaseThread(void) {
	mgl_set_current(0, 0, 0);
	return ok();
}
static void finish(void) {
	mgl_lock();
	mgl_begin(MGL_OP_GL_FINISH_SYNC, 1);
	mgl_end_sync();
	mgl_unlock();
}
EXPORT EGLBoolean eglWaitClient(void) { return finish(), ok(); }
EXPORT EGLBoolean eglWaitGL(void) { return finish(), ok(); }
EXPORT EGLBoolean eglWaitNative(EGLint engine) { return (void)engine, ok(); }
EXPORT EGLint eglGetError(void) {
	EGLint e = last_error;
	last_error = EGL_SUCCESS;
	return e;
}

/* ---- EGL_KHR_fence_sync ---- */
EXPORT EGLSyncKHR eglCreateSyncKHR(EGLDisplay dpy, EGLenum type, const EGLint *a) {
	(void)dpy, (void)a;
	if (type != EGL_SYNC_FENCE_KHR) return (fail(EGL_BAD_ATTRIBUTE), EGL_NO_SYNC_KHR);
	mgl_lock();
	uint32_t id = mgl_new_sync();
	mgl_begin(MGL_OP_EGL_CREATE_SYNC, 0);
	mgl_w32(id), mgl_w32(type);
	mgl_end();
	mgl_unlock();
	ok();
	return (EGLSyncKHR)(uintptr_t)id;
}
EXPORT EGLBoolean eglDestroySyncKHR(EGLDisplay dpy, EGLSyncKHR s) {
	(void)dpy;
	mgl_lock();
	mgl_begin(MGL_OP_EGL_DESTROY_SYNC, 0);
	mgl_w32((uint32_t)(uintptr_t)s);
	mgl_end();
	mgl_unlock();
	return ok();
}
EXPORT EGLint eglClientWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR s, EGLint flags, EGLTimeKHR timeout) {
	(void)dpy;
	mgl_lock();
	mgl_begin(MGL_OP_EGL_CLIENT_WAIT_SYNC, 1);
	mgl_w32((uint32_t)(uintptr_t)s), mgl_w32((uint32_t)flags), mgl_w64(timeout);
	struct mgl_reader r = mgl_end_sync();
	EGLint res = mgl_i32(&r);
	mgl_unlock();
	ok();
	return res;
}
EXPORT EGLBoolean eglGetSyncAttribKHR(EGLDisplay dpy, EGLSyncKHR s, EGLint attr, EGLint *value) {
	if (!value) return fail(EGL_BAD_PARAMETER);
	switch (attr) {
	case EGL_SYNC_TYPE_KHR: *value = EGL_SYNC_FENCE_KHR; return ok();
	case EGL_SYNC_CONDITION_KHR: *value = EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR; return ok();
	case EGL_SYNC_STATUS_KHR:
		*value = eglClientWaitSyncKHR(dpy, s, 0, 0) == EGL_CONDITION_SATISFIED_KHR ? EGL_SIGNALED_KHR : EGL_UNSIGNALED_KHR;
		return ok();
	}
	return fail(EGL_BAD_ATTRIBUTE);
}

/* ---- proc lookup ---- */
struct mgl_proc {
	const char *name;
	void *fn;
};
extern const struct mgl_proc mgl_gl_procs[];
#define P(f) {#f, (void *)f}
static const struct mgl_proc egl_procs[] = {
	P(eglGetDisplay), P(eglGetPlatformDisplayEXT), P(eglGetPlatformDisplay), P(eglInitialize), P(eglTerminate),
	P(eglQueryString), P(eglGetConfigs), P(eglChooseConfig), P(eglGetConfigAttrib), P(eglCreateWindowSurface),
	P(eglCreatePlatformWindowSurfaceEXT), P(eglCreatePbufferSurface), P(eglCreatePixmapSurface),
	P(eglCreatePbufferFromClientBuffer), P(eglDestroySurface), P(eglQuerySurface), P(eglSurfaceAttrib),
	P(eglBindTexImage), P(eglReleaseTexImage), P(eglCopyBuffers), P(eglSwapBuffers), P(eglSwapInterval),
	P(eglBindAPI), P(eglQueryAPI), P(eglCreateContext), P(eglDestroyContext), P(eglMakeCurrent),
	P(eglGetCurrentContext), P(eglGetCurrentSurface), P(eglGetCurrentDisplay), P(eglQueryContext),
	P(eglReleaseThread), P(eglWaitClient), P(eglWaitGL), P(eglWaitNative), P(eglGetError),
	P(eglCreateSyncKHR), P(eglDestroySyncKHR), P(eglClientWaitSyncKHR), P(eglGetSyncAttribKHR),
	{NULL, NULL}};
EXPORT __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
	for (const struct mgl_proc *p = egl_procs; p->name; p++)
		if (!strcmp(p->name, name)) return (__eglMustCastToProperFunctionPointerType)p->fn;
	for (const struct mgl_proc *p = mgl_gl_procs; p->name; p++)
		if (!strcmp(p->name, name)) return (__eglMustCastToProperFunctionPointerType)p->fn;
	return NULL;
}
