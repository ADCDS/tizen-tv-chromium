// glprobe: M0 of the "Chromium on the Mali-450" plan. A native (soft-float,
// glibc 2.20) Tizen client that opens a wl_shell window through the TV's own
// libwayland-client, gets an OpenGL ES 2.0 context from libMali through
// wayland-egl, prints the GPU's capabilities and benchmarks:
//   1. bare swaps                       (presentation ceiling)
//   2. full-screen textured quad        (fill rate at the window size)
//   3. same + full texture upload/frame (what Chromium tile uploads cost)
// The TV's libwayland-client predates wl_proxy_marshal_flags, so requests are
// issued with wl_proxy_marshal(_constructor) directly instead of the inline
// protocol helpers from modern headers.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client-core.h>
#include <wayland-egl-core.h>

extern const struct wl_interface wl_registry_interface, wl_compositor_interface, wl_surface_interface,
    wl_shell_interface, wl_shell_surface_interface;

static struct wl_proxy *compositor, *shell;

static void reg_global(void *d, struct wl_proxy *reg, uint32_t name, const char *iface, uint32_t ver) {
	(void)d;
	(void)ver;
	if (!strcmp(iface, "wl_compositor"))
		compositor = wl_proxy_marshal_constructor_versioned(reg, 0, &wl_compositor_interface, 3, name,
		                                                    wl_compositor_interface.name, 3, NULL);
	else if (!strcmp(iface, "wl_shell"))
		shell = wl_proxy_marshal_constructor_versioned(reg, 0, &wl_shell_interface, 1, name,
		                                               wl_shell_interface.name, 1, NULL);
}
static void reg_remove(void *d, struct wl_proxy *reg, uint32_t name) { (void)d, (void)reg, (void)name; }
static const struct { void (*global)(void *, struct wl_proxy *, uint32_t, const char *, uint32_t);
	void (*remove)(void *, struct wl_proxy *, uint32_t); } reg_listener = {reg_global, reg_remove};

static void ss_ping(void *d, struct wl_proxy *ss, uint32_t serial) { (void)d; wl_proxy_marshal(ss, 0, serial); }
static void ss_configure(void *d, struct wl_proxy *ss, uint32_t e, int32_t w, int32_t h) { (void)d, (void)ss, (void)e, (void)w, (void)h; }
static void ss_popup_done(void *d, struct wl_proxy *ss) { (void)d, (void)ss; }
static const struct { void (*ping)(void *, struct wl_proxy *, uint32_t);
	void (*configure)(void *, struct wl_proxy *, uint32_t, int32_t, int32_t);
	void (*popup_done)(void *, struct wl_proxy *); } ss_listener = {ss_ping, ss_configure, ss_popup_done};

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static GLuint shader(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		fprintf(stderr, "shader: %s\n", log);
		exit(1);
	}
	return s;
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
#define STEP(x) fprintf(stderr, "step: %s\n", x)
	int W = argc > 1 ? atoi(argv[1]) : 1920, H = argc > 2 ? atoi(argv[2]) : 1080;
	int TW = 1280, TH = 720, N = 120;
	STEP("connect");
	struct wl_display *display = wl_display_connect("wayland-0") /* old libwayland: name is relative to $XDG_RUNTIME_DIR (/run) */;
	if (!display) { fprintf(stderr, "wayland connect failed\n"); return 1; }
	struct wl_proxy *registry = wl_proxy_marshal_constructor((struct wl_proxy *)display, 1, &wl_registry_interface, NULL);
	wl_proxy_add_listener(registry, (void (**)(void))&reg_listener, NULL);
	STEP("roundtrip");
	wl_display_roundtrip(display);
	STEP("registry done");
	if (!compositor || !shell) { fprintf(stderr, "missing wl_compositor/wl_shell\n"); return 1; }
	struct wl_proxy *surface = wl_proxy_marshal_constructor(compositor, 0, &wl_surface_interface, NULL);
	struct wl_proxy *ss = wl_proxy_marshal_constructor(shell, 0, &wl_shell_surface_interface, NULL, surface);
	wl_proxy_add_listener(ss, (void (**)(void))&ss_listener, NULL);
	wl_proxy_marshal(ss, 3); // set_toplevel
	STEP("wl_egl_window_create");
	struct wl_egl_window *ewin = wl_egl_window_create((struct wl_surface *)surface, W, H);

	STEP("eglGetDisplay");
	EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)display);
	EGLint maj, min;
	STEP("eglInitialize");
	if (!eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "eglInitialize failed 0x%x\n", eglGetError()); return 1; }
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint cattr[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
	                  EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
	EGLConfig cfg;
	EGLint ncfg;
	if (!eglChooseConfig(dpy, cattr, &cfg, 1, &ncfg) || ncfg < 1) { fprintf(stderr, "no config\n"); return 1; }
	EGLint ctxattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	STEP("eglCreateContext");
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
	STEP("eglCreateWindowSurface");
	EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)ewin, NULL);
	if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "context/surface failed 0x%x\n", eglGetError());
		return 1;
	}
	eglSwapInterval(dpy, 1);

	printf("EGL %d.%d  vendor=%s\n  version=%s\n  extensions=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR),
	       eglQueryString(dpy, EGL_VERSION), eglQueryString(dpy, EGL_EXTENSIONS));
	printf("GL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\nGLSL=%s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
	       glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
	printf("GL_EXTENSIONS=%s\n", glGetString(GL_EXTENSIONS));
	GLint v[2], p;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, v);
	printf("max texture %d", v[0]);
	glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, v);
	printf(", max renderbuffer %d", v[0]);
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, v);
	printf(", fragment texture units %d", v[0]);
	glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, v);
	printf(", vertex attribs %d\n", v[0]);
	glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, v, &p);
	printf("fragment highp float: range %d,%d precision %d\n", v[0], v[1], p);
	glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_MEDIUM_FLOAT, v, &p);
	printf("fragment mediump float: range %d,%d precision %d\n", v[0], v[1], p);
	fflush(stdout);

	GLuint prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, "attribute vec2 p; varying vec2 uv;"
	                                              "void main(){ uv = p*0.5+0.5; gl_Position = vec4(p,0.0,1.0); }"));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, "precision mediump float; varying vec2 uv; uniform sampler2D t;"
	                                                "void main(){ gl_FragColor = texture2D(t, vec2(uv.x, 1.0-uv.y)); }"));
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	glUseProgram(prog);
	static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray(0);
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	uint32_t *pix = malloc((size_t)TW * TH * 4);
	for (int i = 0; i < TW * TH; i++) pix[i] = 0xff000000u | (uint32_t)((i % TW) * 255 / TW) << 8 | (uint32_t)((i / TW) * 255 / TH);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TW, TH, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);

	if (argc > 3) { /* replay Chromium's pattern: draw, flush, swap, glFinish */
		eglSwapInterval(dpy, atoi(argv[3]));
		for (int f = 0; f < 5; f++) {
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glFlush();
			double a = now();
			eglSwapBuffers(dpy, surf);
			double b = now();
			fprintf(stderr, "frame %d: swap %.1f ms, ", f, (b - a) * 1e3);
			glFinish();
			fprintf(stderr, "glFinish %.1f ms\n", (now() - b) * 1e3);
			wl_display_dispatch_pending(display);
		}
		return 0;
	}
	for (int test = 0; test < 3; test++) {
		double t0 = now();
		for (int f = 0; f < N; f++) {
			if (test == 0) {
				glClearColor((f % 60) / 60.0f, 0.2f, 0.4f, 1.0f);
				glClear(GL_COLOR_BUFFER_BIT);
			} else {
				if (test == 2) {
					for (int y = 0; y < TH; y++) pix[y * TW + (f * 7) % TW] = 0xffffffffu; // touch a column
					glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TW, TH, GL_RGBA, GL_UNSIGNED_BYTE, pix);
				}
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			}
			eglSwapBuffers(dpy, surf);
			wl_display_dispatch_pending(display);
		}
		glFinish();
		double dt = now() - t0;
		const char *name[] = {"swap only", "textured quad", "quad + 1280x720 upload/frame"};
		printf("%-30s %5.1f fps (%dx%d window)\n", name[test], N / dt, W, H);
		fflush(stdout);
	}
	eglDestroySurface(dpy, surf);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
	wl_egl_window_destroy(ewin);
	wl_display_disconnect(display);
	return 0;
}
