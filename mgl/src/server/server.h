#ifndef MGL_SERVER_H
#define MGL_SERVER_H
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>

#include "../proto.h"

/* client id -> real GL name */
struct nmap {
	uint32_t *v;
	uint32_t n;
};
uint32_t nmap_get(struct nmap *m, uint32_t id);
void nmap_set(struct nmap *m, uint32_t id, uint32_t real);

enum { NS_TEXTURE, NS_BUFFER, NS_RENDERBUFFER, NS_PROGRAM, NS_FRAMEBUFFER, NS_VERTEX_ARRAY, NS_COUNT };

struct sgroup { /* share group: textures, buffers, renderbuffers, programs+shaders */
	struct nmap ns[4];
	struct nmap shader_type; /* client shader id -> GL_VERTEX_SHADER/GL_FRAGMENT_SHADER */
};
struct sctx {
	EGLContext egl;
	struct sgroup *group;
	struct nmap fbo, vao; /* per-context namespaces */
};
struct ssurface {
	EGLSurface egl;
	void *win; /* platform window handle, NULL for pbuffers */
	int w, h;
};

/* The context the decoded GL call runs in, and helpers the GL decoders use. */
extern struct sctx *cur_ctx;
uint32_t map_name(int ns, uint32_t id);       /* client id -> real (0 stays 0) */
uint32_t create_name(int ns, uint32_t id);     /* make a real object for a client id */
void forget_name(int ns, uint32_t id);
void note_shader_type(uint32_t id, uint32_t type);
uint32_t shader_type(uint32_t id);

/* Reply building for sync ops. */
void reply_u32(uint32_t v);
void reply_f32(float v);
void reply_blob(const void *p, uint32_t n);
void *reply_space(uint32_t n);

/* GL decoders (generated + special). Returns 0 if the op is unknown. */
int dispatch_gl(uint16_t op, struct mgl_reader *r);

/* Platform: the TV uses Wayland windows on the Mali, the PC Mesa pbuffers. */
EGLDisplay plat_init(void);
EGLSurface plat_create_window_surface(EGLDisplay dpy, EGLConfig cfg, int w, int h, void **win);
void plat_resize_window(void *win, int w, int h);
void plat_destroy_window(void *win);
void plat_before_swap(struct ssurface *s); /* e.g. PC: dump the frame for checking */
void plat_after_swap(struct ssurface *s);   /* e.g. TV: dispatch Wayland events */
int plat_window_visible(void *win);          /* 0: hidden or covered, skip swaps */
#define PLAT_MAX_FDS 12
int plat_poll_fds(struct pollfd *pf, int max); /* fds for the server's poll loop */
void plat_poll_ready(struct pollfd *pf, int n); /* after poll: handle what's ready */

extern int verbose;
#endif
