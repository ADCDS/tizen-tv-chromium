/* mgl-server platform for testing on a PC with Mesa: window surfaces become
 * pbuffers; with MGL_DUMP_DIR set, every MGL_DUMP_EVERY-th frame is written
 * there as a PPM so the composited result can be inspected. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "server.h"

static EGLDisplay dpy;

EGLDisplay plat_init(void) {
	dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint ma, mi;
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &ma, &mi)) {
		fprintf(stderr, "mgl-server(pc): no EGL display\n");
		return EGL_NO_DISPLAY;
	}
	return dpy;
}
EGLSurface plat_create_window_surface(EGLDisplay d, EGLConfig cfg, int w, int h, void **win) {
	EGLint a[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
	*win = NULL;
	return eglCreatePbufferSurface(d, cfg, a);
}
void plat_resize_window(void *win, int w, int h) { (void)win, (void)w, (void)h; }
void plat_destroy_window(void *win) { (void)win; }

void plat_before_swap(struct ssurface *s) {
	static unsigned long frame;
	const char *dir = getenv("MGL_DUMP_DIR");
	int every = getenv("MGL_DUMP_EVERY") ? atoi(getenv("MGL_DUMP_EVERY")) : 30;
	if (!dir || frame++ % (unsigned long)(every > 0 ? every : 30)) return;
	unsigned char *px = malloc((size_t)s->w * s->h * 4);
	GLint align = 4, fbo = 0;
	glGetIntegerv(GL_PACK_ALIGNMENT, &align);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, s->w, s->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glPixelStorei(GL_PACK_ALIGNMENT, align);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
	char path[512];
	snprintf(path, sizeof path, "%s/frame-%05lu.ppm", dir, frame - 1);
	FILE *f = fopen(path, "wb");
	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
		for (int y = s->h - 1; y >= 0; y--)
			for (int x = 0; x < s->w; x++) fwrite(px + ((size_t)y * s->w + x) * 4, 1, 3, f);
		fclose(f);
	}
	free(px);
}
void plat_after_swap(struct ssurface *s) { (void)s; }
int plat_window_visible(void *win) { (void)win; return 1; }
int plat_poll_fds(struct pollfd *pf, int max) { (void)pf, (void)max; return 0; }
void plat_poll_ready(struct pollfd *pf, int n) { (void)pf, (void)n; }
