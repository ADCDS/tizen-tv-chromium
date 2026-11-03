#ifndef MGL_CLIENT_CORE_H
#define MGL_CLIENT_CORE_H
#include <pthread.h>
#include <stddef.h>

#include "../proto.h"

#define MGL_MAX_CTX 1024

/* Object namespaces. Textures, buffers, renderbuffers and program/shader
 * names are shared by a share group; framebuffers and VAOs are per context. */
enum { MGL_NS_TEXTURE, MGL_NS_BUFFER, MGL_NS_RENDERBUFFER, MGL_NS_PROGRAM, MGL_NS_FRAMEBUFFER,
       MGL_NS_VERTEX_ARRAY, MGL_NS_COUNT };

struct mgl_group {
	int used;
	uint32_t next[MGL_NS_COUNT];
};
struct mgl_ctx {
	uint32_t id, group;
	uint32_t next[MGL_NS_COUNT];
	void *arrays; /* client-memory vertex array tracking (arrays.c) */
};
struct mgl_cur {
	uint32_t ctx, draw, read;
};

void mgl_die(const char *what) __attribute__((noreturn));
void mgl_lock(void);
void mgl_unlock(void);
void mgl_begin(uint16_t op, int sync);
void mgl_end(void);
struct mgl_reader mgl_end_sync(void);
void mgl_flush(void);
void mgl_w32(uint32_t v);
void mgl_wf(float f);
void mgl_w64(uint64_t v);
void mgl_wblob(const void *p, uint32_t len);

struct mgl_cur *mgl_current(void);
void mgl_set_current(uint32_t ctx, uint32_t draw, uint32_t read);
struct mgl_ctx *mgl_ctx_get(uint32_t id);
uint32_t mgl_new_ctx(uint32_t share);
uint32_t mgl_new_surface(void);
uint32_t mgl_new_sync(void);
uint32_t mgl_alloc_name(int ns);
#endif
