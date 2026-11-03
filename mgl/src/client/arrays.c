/* mgl client: vertex arrays and draws. ES 2.0 lets attributes and indices come
 * from client memory (no buffer bound), and Skia/ANGLE do that. Such pointers
 * are meaningless in the server, so the client tracks where each attribute
 * comes from and, at draw time, ships the client-memory data with the draw.
 * Element buffer contents are shadowed so indexed draws with client-memory
 * attributes know how many vertices to send. */
#define _GNU_SOURCE
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gl_ops.h"
#include "core.h"

#define MAXATTR 16
struct attrib {
	int enabled, client;
	GLint size;
	GLenum type;
	GLboolean norm;
	GLsizei stride;
	const void *ptr;
};
struct vao {
	GLuint element_buffer;
	struct attrib a[MAXATTR];
};
struct shadow { /* copy of an element buffer's contents */
	uint8_t *data;
	size_t size;
};
struct cstate {
	GLuint array_buffer, vao_id;
	struct vao def;
	struct vao **vaos; /* by client VAO id */
	uint32_t nvaos;
};
/* element buffer shadows, by share group then buffer id */
static struct shadow *shadows[MGL_MAX_CTX];
static uint32_t nshadows[MGL_MAX_CTX];

static struct cstate *state(void) {
	struct mgl_ctx *c = mgl_ctx_get(mgl_current()->ctx);
	if (!c) return NULL;
	if (!c->arrays) c->arrays = calloc(1, sizeof(struct cstate));
	return c->arrays;
}
static struct vao *cur_vao(struct cstate *s) {
	if (!s->vao_id) return &s->def;
	if (s->vao_id >= s->nvaos) {
		uint32_t n = s->nvaos ? s->nvaos : 64;
		while (n <= s->vao_id) n *= 2;
		s->vaos = realloc(s->vaos, n * sizeof *s->vaos);
		memset(s->vaos + s->nvaos, 0, (n - s->nvaos) * sizeof *s->vaos);
		s->nvaos = n;
	}
	if (!s->vaos[s->vao_id]) s->vaos[s->vao_id] = calloc(1, sizeof(struct vao));
	return s->vaos[s->vao_id];
}
static struct shadow *shadow_get(GLuint buf, int create) {
	struct mgl_ctx *c = mgl_ctx_get(mgl_current()->ctx);
	if (!c || !buf) return NULL;
	uint32_t g = c->group;
	if (buf >= nshadows[g]) {
		if (!create) return NULL;
		uint32_t n = nshadows[g] ? nshadows[g] : 256;
		while (n <= buf) n *= 2;
		shadows[g] = realloc(shadows[g], n * sizeof(struct shadow));
		memset(shadows[g] + nshadows[g], 0, (n - nshadows[g]) * sizeof(struct shadow));
		nshadows[g] = n;
	}
	return &shadows[g][buf];
}

/* ---- hooks called by the generated encoders ---- */
void mgl_hook_glBindBuffer(GLenum target, GLuint buffer) {
	struct cstate *s = state();
	if (!s) return;
	if (target == GL_ARRAY_BUFFER) s->array_buffer = buffer;
	if (target == GL_ELEMENT_ARRAY_BUFFER) cur_vao(s)->element_buffer = buffer;
}
void mgl_hook_glBindVertexArrayOES(GLuint array) {
	struct cstate *s = state();
	if (s) s->vao_id = array;
}
void mgl_hook_glEnableVertexAttribArray(GLuint i) {
	struct cstate *s = state();
	if (s && i < MAXATTR) cur_vao(s)->a[i].enabled = 1;
}
void mgl_hook_glDisableVertexAttribArray(GLuint i) {
	struct cstate *s = state();
	if (s && i < MAXATTR) cur_vao(s)->a[i].enabled = 0;
}
void mgl_hook_glDeleteBuffers(GLsizei n, const GLuint *bufs) {
	struct cstate *s = state();
	for (GLsizei i = 0; s && i < n; i++) {
		if (s->array_buffer == bufs[i]) s->array_buffer = 0;
		if (cur_vao(s)->element_buffer == bufs[i]) cur_vao(s)->element_buffer = 0;
		struct shadow *sh = shadow_get(bufs[i], 0);
		if (sh) free(sh->data), sh->data = NULL, sh->size = 0;
	}
}
void mgl_hook_glDeleteVertexArraysOES(GLsizei n, const GLuint *ids) {
	struct cstate *s = state();
	for (GLsizei i = 0; s && i < n; i++) {
		if (ids[i] < s->nvaos && s->vaos[ids[i]]) free(s->vaos[ids[i]]), s->vaos[ids[i]] = NULL;
		if (s->vao_id == ids[i]) s->vao_id = 0;
	}
}
/* Buffer uploads: keep a copy (called from gl_special.c). Any buffer can later
 * be bound as the element buffer: Skia fills its shared quad index buffer
 * through GL_ARRAY_BUFFER, so uploads through either binding are kept. */
void mgl_shadow_buffer(GLenum target, GLintptr offset, GLsizeiptr size, const void *data, int whole) {
	struct cstate *s = state();
	if (!s) return;
	GLuint buf = target == GL_ELEMENT_ARRAY_BUFFER ? cur_vao(s)->element_buffer
	             : target == GL_ARRAY_BUFFER       ? s->array_buffer
	                                               : 0;
	struct shadow *sh = shadow_get(buf, 1);
	if (!sh) return;
	if (whole) {
		free(sh->data);
		sh->data = calloc(1, (size_t)size + 1);
		sh->size = (size_t)size;
	}
	if (data && sh->data && (size_t)(offset + size) <= sh->size) memcpy(sh->data + offset, data, (size_t)size);
}

/* ---- pointers and draws ---- */
static uint32_t type_size(GLenum t) {
	switch (t) {
	case GL_BYTE:
	case GL_UNSIGNED_BYTE: return 1;
	case GL_SHORT:
	case GL_UNSIGNED_SHORT:
	case GL_HALF_FLOAT_OES: return 2;
	default: return 4;
	}
}

GL_APICALL void GL_APIENTRY glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride,
                                                  const void *ptr) {
	struct cstate *s = state();
	if (s && index < MAXATTR) {
		struct attrib *a = &cur_vao(s)->a[index];
		a->client = s->array_buffer == 0;
		a->size = size, a->type = type, a->norm = norm, a->stride = stride, a->ptr = ptr;
		if (a->client) return; /* sent with each draw instead */
	}
	mgl_lock();
	mgl_begin(OP_glVertexAttribPointer, 0);
	mgl_w32(index), mgl_w32((uint32_t)size), mgl_w32(type), mgl_w32(norm), mgl_w32((uint32_t)stride);
	mgl_w64((uint64_t)(uintptr_t)ptr);
	mgl_end();
	mgl_unlock();
}

/* Append the client-memory attributes covering vertices [0, nverts). */
static void write_client_attribs(struct cstate *s, uint32_t nverts) {
	struct vao *v = s ? cur_vao(s) : NULL;
	uint32_t n = 0;
	for (int i = 0; v && i < MAXATTR; i++) n += v->a[i].enabled && v->a[i].client && v->a[i].ptr;
	mgl_w32(n);
	for (int i = 0; n && i < MAXATTR; i++) {
		struct attrib *a = &v->a[i];
		if (!(a->enabled && a->client && a->ptr)) continue;
		uint32_t elem = (uint32_t)a->size * type_size(a->type);
		uint32_t stride = a->stride ? (uint32_t)a->stride : elem;
		uint32_t bytes = nverts ? (nverts - 1) * stride + elem : 0;
		mgl_w32((uint32_t)i), mgl_w32((uint32_t)a->size), mgl_w32(a->type), mgl_w32(a->norm), mgl_w32((uint32_t)a->stride);
		mgl_wblob(a->ptr, bytes);
	}
}
static int has_client_attribs(struct cstate *s) {
	struct vao *v = s ? cur_vao(s) : NULL;
	for (int i = 0; v && i < MAXATTR; i++)
		if (v->a[i].enabled && v->a[i].client && v->a[i].ptr) return 1;
	return 0;
}
static uint32_t max_index(const void *idx, GLsizei count, GLenum type) {
	uint32_t m = 0;
	for (GLsizei i = 0; idx && i < count; i++) {
		uint32_t v = type == GL_UNSIGNED_BYTE ? ((const uint8_t *)idx)[i]
		             : type == GL_UNSIGNED_SHORT ? ((const uint16_t *)idx)[i]
		                                         : ((const uint32_t *)idx)[i];
		if (v > m) m = v;
	}
	return m;
}

GL_APICALL void GL_APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count) {
	struct cstate *s = state();
	mgl_lock();
	mgl_begin(OP_glDrawArrays, 0);
	mgl_w32(mode), mgl_w32((uint32_t)first), mgl_w32((uint32_t)count);
	write_client_attribs(s, count > 0 ? (uint32_t)(first + count) : 0);
	mgl_end();
	mgl_unlock();
}

GL_APICALL void GL_APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
	struct cstate *s = state();
	GLuint eb = s ? cur_vao(s)->element_buffer : 0;
	uint32_t isize = type_size(type);
	const void *idx = indices; /* where the index values live, for the vertex range */
	if (eb) {
		struct shadow *sh = shadow_get(eb, 0);
		uintptr_t off = (uintptr_t)indices;
		idx = sh && sh->data && off + (size_t)count * isize <= sh->size ? sh->data + off : NULL;
	}
	int client_attribs = has_client_attribs(s);
	uint32_t nverts = client_attribs && idx ? max_index(idx, count, type) + 1 : 0;
	if (client_attribs && !idx && count > 0) {
		static int warned;
		if (!warned++) fprintf(stderr, "mgl: indexed draw with client arrays but unknown indices (buffer %u)\n", eb);
	}
	mgl_lock();
	mgl_begin(OP_glDrawElements, 0);
	mgl_w32(mode), mgl_w32((uint32_t)count), mgl_w32(type), mgl_w32(eb == 0);
	if (eb) mgl_w64((uint64_t)(uintptr_t)indices);
	else mgl_wblob(indices, (uint32_t)count * isize);
	if (client_attribs) write_client_attribs(s, nverts);
	else mgl_w32(0);
	mgl_end();
	mgl_unlock();
}
