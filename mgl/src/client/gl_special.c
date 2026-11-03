/* mgl client: GL entry points that need more than mechanical encoding. */
#define _GNU_SOURCE
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdlib.h>
#include <string.h>

#include "../gl_ops.h"
#include "core.h"

#define API GL_APICALL
#define E GL_APIENTRY

static GLint unpack_align = 4, pack_align = 4;

static uint32_t bytes_per_pixel(GLenum format, GLenum type) {
	switch (type) {
	case GL_UNSIGNED_SHORT_5_6_5:
	case GL_UNSIGNED_SHORT_4_4_4_4:
	case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
	case GL_UNSIGNED_INT_24_8_OES: return 4;
	case GL_UNSIGNED_SHORT: return format == GL_DEPTH_COMPONENT ? 2 : 2;
	case GL_UNSIGNED_INT: return 4;
	case GL_FLOAT: return 4 * (format == GL_RGBA ? 4 : format == GL_RGB ? 3 : format == GL_LUMINANCE_ALPHA ? 2 : 1);
	case GL_HALF_FLOAT_OES: return 2 * (format == GL_RGBA ? 4 : format == GL_RGB ? 3 : format == GL_LUMINANCE_ALPHA ? 2 : 1);
	}
	switch (format) { /* GL_UNSIGNED_BYTE */
	case GL_RGBA:
	case GL_BGRA_EXT: return 4;
	case GL_RGB: return 3;
	case GL_LUMINANCE_ALPHA: return 2;
	default: return 1; /* ALPHA, LUMINANCE */
	}
}
/* Bytes GL reads/writes for a w x h image with the given row alignment. */
static uint32_t image_size(GLsizei w, GLsizei h, GLenum format, GLenum type, GLint align) {
	if (w <= 0 || h <= 0) return 0;
	uint32_t row = (uint32_t)w * bytes_per_pixel(format, type);
	uint32_t stride = (row + (uint32_t)align - 1) / (uint32_t)align * (uint32_t)align;
	return stride * (uint32_t)(h - 1) + row;
}

#define BEGIN(op, sync) \
	mgl_lock(); \
	mgl_begin(op, sync)
#define END() \
	mgl_end(); \
	mgl_unlock()

API void E glPixelStorei(GLenum pname, GLint param) {
	if (pname == GL_UNPACK_ALIGNMENT) unpack_align = param;
	if (pname == GL_PACK_ALIGNMENT) pack_align = param;
	BEGIN(OP_glPixelStorei, 0);
	mgl_w32(pname), mgl_w32((uint32_t)param);
	END();
}

API void E glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum format,
                        GLenum type, const void *pixels) {
	BEGIN(OP_glTexImage2D, 0);
	mgl_w32(target), mgl_w32((uint32_t)level), mgl_w32((uint32_t)ifmt), mgl_w32((uint32_t)w), mgl_w32((uint32_t)h);
	mgl_w32((uint32_t)border), mgl_w32(format), mgl_w32(type);
	mgl_wblob(pixels, pixels ? image_size(w, h, format, type, unpack_align) : 0);
	END();
}
API void E glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                           GLenum type, const void *pixels) {
	BEGIN(OP_glTexSubImage2D, 0);
	mgl_w32(target), mgl_w32((uint32_t)level), mgl_w32((uint32_t)x), mgl_w32((uint32_t)y);
	mgl_w32((uint32_t)w), mgl_w32((uint32_t)h), mgl_w32(format), mgl_w32(type);
	mgl_wblob(pixels, pixels ? image_size(w, h, format, type, unpack_align) : 0);
	END();
}
API void E glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border,
                                  GLsizei size, const void *data) {
	BEGIN(OP_glCompressedTexImage2D, 0);
	mgl_w32(target), mgl_w32((uint32_t)level), mgl_w32(ifmt), mgl_w32((uint32_t)w), mgl_w32((uint32_t)h);
	mgl_w32((uint32_t)border);
	mgl_wblob(data, data ? (uint32_t)size : 0);
	END();
}
API void E glCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                     GLenum format, GLsizei size, const void *data) {
	BEGIN(OP_glCompressedTexSubImage2D, 0);
	mgl_w32(target), mgl_w32((uint32_t)level), mgl_w32((uint32_t)x), mgl_w32((uint32_t)y);
	mgl_w32((uint32_t)w), mgl_w32((uint32_t)h), mgl_w32(format);
	mgl_wblob(data, data ? (uint32_t)size : 0);
	END();
}
API void E glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels) {
	uint32_t n = image_size(w, h, format, type, pack_align);
	BEGIN(OP_glReadPixels, 1);
	mgl_w32((uint32_t)x), mgl_w32((uint32_t)y), mgl_w32((uint32_t)w), mgl_w32((uint32_t)h);
	mgl_w32(format), mgl_w32(type), mgl_w32(n);
	struct mgl_reader r = mgl_end_sync();
	uint32_t len;
	const void *p = mgl_blob(&r, &len);
	if (pixels && p) memcpy(pixels, p, len < n ? len : n);
	mgl_unlock();
}

void mgl_shadow_buffer(GLenum target, GLintptr offset, GLsizeiptr size, const void *data, int whole);
API void E glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
	mgl_shadow_buffer(target, 0, size, data, 1);
	BEGIN(OP_glBufferData, 0);
	mgl_w32(target), mgl_w64((uint64_t)size), mgl_w32(usage), mgl_w32(data != NULL);
	mgl_wblob(data, data ? (uint32_t)size : 0);
	END();
}
API void E glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
	mgl_shadow_buffer(target, offset, size, data, 0);
	BEGIN(OP_glBufferSubData, 0);
	mgl_w32(target), mgl_w64((uint64_t)offset);
	mgl_wblob(data, (uint32_t)size);
	END();
}

API void E glShaderSource(GLuint shader, GLsizei count, const GLchar *const *str, const GLint *len) {
	size_t total = 0;
	for (GLsizei i = 0; i < count; i++) total += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
	char *s = malloc(total + 1), *p = s;
	for (GLsizei i = 0; i < count; i++) {
		size_t n = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
		memcpy(p, str[i], n);
		p += n;
	}
	*p = 0;
	BEGIN(OP_glShaderSource, 0);
	mgl_w32(shader);
	mgl_wblob(s, (uint32_t)total + 1);
	END();
	free(s);
}
API void E glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) {
	BEGIN(OP_glBindAttribLocation, 0);
	mgl_w32(program), mgl_w32(index), mgl_wblob(name, (uint32_t)strlen(name) + 1);
	END();
}
static GLint location(uint16_t op, GLuint program, const GLchar *name) {
	BEGIN(op, 1);
	mgl_w32(program), mgl_wblob(name, (uint32_t)strlen(name) + 1);
	struct mgl_reader r = mgl_end_sync();
	GLint v = mgl_i32(&r);
	mgl_unlock();
	return v;
}
API GLint E glGetAttribLocation(GLuint program, const GLchar *name) { return location(OP_glGetAttribLocation, program, name); }
API GLint E glGetUniformLocation(GLuint program, const GLchar *name) { return location(OP_glGetUniformLocation, program, name); }

API GLuint E glCreateProgram(void) {
	mgl_lock();
	GLuint id = mgl_alloc_name(MGL_NS_PROGRAM);
	mgl_begin(OP_glCreateProgram, 0);
	mgl_w32(id);
	END();
	return id;
}
API GLuint E glCreateShader(GLenum type) {
	mgl_lock();
	GLuint id = mgl_alloc_name(MGL_NS_PROGRAM);
	mgl_begin(OP_glCreateShader, 0);
	mgl_w32(id), mgl_w32(type);
	END();
	return id;
}

API void E glFlush(void) {
	BEGIN(OP_glFlush, 0);
	mgl_end();
	mgl_flush();
	mgl_unlock();
}
API void E glFinish(void) {
	BEGIN(OP_glFinish, 1);
	mgl_end_sync();
	mgl_unlock();
}
API GLenum E glGetError(void) {
	BEGIN(OP_glGetError, 1);
	struct mgl_reader r = mgl_end_sync();
	GLenum e = mgl_u32(&r);
	mgl_unlock();
	return e;
}

/* ---- queries: server replies with u32 count, then count 32-bit values ---- */
static void query(uint16_t op, uint32_t a, uint32_t b, uint32_t c, void *out, uint32_t max) {
	BEGIN(op, 1);
	mgl_w32(a), mgl_w32(b), mgl_w32(c);
	struct mgl_reader r = mgl_end_sync();
	uint32_t n = mgl_u32(&r);
	for (uint32_t i = 0; i < n; i++) {
		uint32_t v = mgl_u32(&r);
		if (out && i < max) memcpy((uint8_t *)out + 4 * i, &v, 4);
	}
	mgl_unlock();
}
#define BIG 1024
API void E glGetIntegerv(GLenum p, GLint *v) { query(OP_glGetIntegerv, p, 0, 0, v, BIG); }
API void E glGetFloatv(GLenum p, GLfloat *v) { query(OP_glGetFloatv, p, 0, 0, v, BIG); }
API void E glGetBooleanv(GLenum p, GLboolean *v) {
	GLint tmp[64] = {0};
	BEGIN(OP_glGetIntegerv, 1); /* booleans travel as ints */
	mgl_w32(p), mgl_w32(0), mgl_w32(0);
	struct mgl_reader r = mgl_end_sync();
	uint32_t n = mgl_u32(&r);
	for (uint32_t i = 0; i < n; i++) tmp[i < 64 ? i : 63] = mgl_i32(&r);
	mgl_unlock();
	for (uint32_t i = 0; i < n && i < 64; i++) v[i] = tmp[i] != 0;
}
API void E glGetTexParameterfv(GLenum t, GLenum p, GLfloat *v) { query(OP_glGetTexParameterfv, t, p, 0, v, 4); }
API void E glGetTexParameteriv(GLenum t, GLenum p, GLint *v) { query(OP_glGetTexParameteriv, t, p, 0, v, 4); }
API void E glGetBufferParameteriv(GLenum t, GLenum p, GLint *v) { query(OP_glGetBufferParameteriv, t, p, 0, v, 1); }
API void E glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum p, GLint *v) {
	query(OP_glGetFramebufferAttachmentParameteriv, t, a, p, v, 1);
}
API void E glGetRenderbufferParameteriv(GLenum t, GLenum p, GLint *v) { query(OP_glGetRenderbufferParameteriv, t, p, 0, v, 1); }
API void E glGetShaderiv(GLuint s, GLenum p, GLint *v) { query(OP_glGetShaderiv, s, p, 0, v, 1); }
API void E glGetProgramiv(GLuint s, GLenum p, GLint *v) { query(OP_glGetProgramiv, s, p, 0, v, 1); }
API void E glGetVertexAttribfv(GLuint i, GLenum p, GLfloat *v) { query(OP_glGetVertexAttribfv, i, p, 0, v, 4); }
API void E glGetVertexAttribiv(GLuint i, GLenum p, GLint *v) { query(OP_glGetVertexAttribiv, i, p, 0, v, 4); }
API void E glGetVertexAttribPointerv(GLuint i, GLenum p, void **v) {
	uint32_t off = 0;
	query(OP_glGetVertexAttribPointerv, i, p, 0, &off, 1);
	if (v) *v = (void *)(uintptr_t)off;
}
API void E glGetUniformfv(GLuint prog, GLint loc, GLfloat *v) { query(OP_glGetUniformfv, prog, (uint32_t)loc, 0, v, 16); }
API void E glGetUniformiv(GLuint prog, GLint loc, GLint *v) { query(OP_glGetUniformiv, prog, (uint32_t)loc, 0, v, 16); }
API void E glGetAttachedShaders(GLuint prog, GLsizei max, GLsizei *count, GLuint *shaders) {
	uint32_t ids[64];
	BEGIN(OP_glGetAttachedShaders, 1);
	mgl_w32(prog), mgl_w32(0), mgl_w32(0);
	struct mgl_reader r = mgl_end_sync();
	uint32_t n = mgl_u32(&r);
	for (uint32_t i = 0; i < n; i++) ids[i < 64 ? i : 63] = mgl_u32(&r);
	mgl_unlock();
	GLsizei k = 0;
	for (; k < (GLsizei)n && k < max && k < 64; k++) shaders[k] = ids[k];
	if (count) *count = k;
}

/* ---- string results ---- */
static void str_query(uint16_t op, uint32_t a, uint32_t b, GLsizei bufsize, GLsizei *length, GLchar *out) {
	BEGIN(op, 1);
	mgl_w32(a), mgl_w32(b), mgl_w32((uint32_t)bufsize);
	struct mgl_reader r = mgl_end_sync();
	uint32_t len;
	const char *s = mgl_blob(&r, &len);
	GLsizei n = 0;
	if (out && bufsize > 0) {
		n = (GLsizei)len < bufsize - 1 ? (GLsizei)len : bufsize - 1;
		if (s) memcpy(out, s, (size_t)n);
		out[n] = 0;
	}
	if (length) *length = n;
	mgl_unlock();
}
API void E glGetShaderInfoLog(GLuint s, GLsizei n, GLsizei *len, GLchar *log) { str_query(OP_glGetShaderInfoLog, s, 0, n, len, log); }
API void E glGetProgramInfoLog(GLuint s, GLsizei n, GLsizei *len, GLchar *log) { str_query(OP_glGetProgramInfoLog, s, 0, n, len, log); }
API void E glGetShaderSource(GLuint s, GLsizei n, GLsizei *len, GLchar *src) { str_query(OP_glGetShaderSource, s, 0, n, len, src); }
static void active(uint16_t op, GLuint prog, GLuint idx, GLsizei bufsize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
	BEGIN(op, 1);
	mgl_w32(prog), mgl_w32(idx), mgl_w32((uint32_t)bufsize);
	struct mgl_reader r = mgl_end_sync();
	GLint sz = mgl_i32(&r);
	GLenum ty = mgl_u32(&r);
	uint32_t len;
	const char *s = mgl_blob(&r, &len);
	if (size) *size = sz;
	if (type) *type = ty;
	GLsizei n = 0;
	if (name && bufsize > 0) {
		n = (GLsizei)len < bufsize - 1 ? (GLsizei)len : bufsize - 1;
		if (s) memcpy(name, s, (size_t)n);
		name[n] = 0;
	}
	if (length) *length = n;
	mgl_unlock();
}
API void E glGetActiveAttrib(GLuint p, GLuint i, GLsizei n, GLsizei *len, GLint *size, GLenum *type, GLchar *name) {
	active(OP_glGetActiveAttrib, p, i, n, len, size, type, name);
}
API void E glGetActiveUniform(GLuint p, GLuint i, GLsizei n, GLsizei *len, GLint *size, GLenum *type, GLchar *name) {
	active(OP_glGetActiveUniform, p, i, n, len, size, type, name);
}

/* GL_EXTENSIONS is filtered down to what the relay implements. */
static const char *allowed_exts[] = {
	"GL_OES_texture_npot", "GL_OES_vertex_array_object", "GL_OES_compressed_ETC1_RGB8_texture",
	"GL_OES_standard_derivatives", "GL_OES_depth24", "GL_OES_depth_texture", "GL_OES_packed_depth_stencil",
	"GL_EXT_texture_format_BGRA8888", "GL_OES_vertex_half_float", "GL_EXT_blend_minmax", "GL_OES_rgb8_rgba8",
	"GL_EXT_multisampled_render_to_texture", "GL_EXT_discard_framebuffer", "GL_EXT_shader_texture_lod",
	"GL_OES_depth_texture_cube_map", "GL_ARM_rgba8", NULL};
API const GLubyte *E glGetString(GLenum name) {
	static char *cache[8];
	int slot = name == GL_VENDOR ? 0 : name == GL_RENDERER ? 1 : name == GL_VERSION ? 2 : name == GL_SHADING_LANGUAGE_VERSION ? 3 : name == GL_EXTENSIONS ? 4 : -1;
	if (slot < 0) return NULL;
	if (cache[slot]) return (const GLubyte *)cache[slot];
	BEGIN(OP_glGetString, 1);
	mgl_w32(name);
	struct mgl_reader r = mgl_end_sync();
	uint32_t len;
	const char *s = mgl_blob(&r, &len);
	char *copy = calloc(1, len + 1);
	if (s) memcpy(copy, s, len);
	mgl_unlock();
	if (name == GL_EXTENSIONS) {
		char *out = calloc(1, len + 2);
		for (char *tok = strtok(copy, " "); tok; tok = strtok(NULL, " "))
			for (int i = 0; allowed_exts[i]; i++)
				if (!strcmp(tok, allowed_exts[i])) {
					strcat(out, tok);
					strcat(out, " ");
				}
		free(copy);
		copy = out;
	}
	cache[slot] = copy;
	return (const GLubyte *)copy;
}

/* ANGLE refuses ES2 without fragment highp; the server compiles highp as
 * mediump on the Mali (see server/gl_special.c). */
API void E glGetShaderPrecisionFormat(GLenum st, GLenum pt, GLint *range, GLint *precision) {
	if (st == GL_FRAGMENT_SHADER && (pt == GL_HIGH_FLOAT || pt == GL_HIGH_INT)) {
		int f = pt == GL_HIGH_FLOAT;
		if (range) range[0] = f ? 127 : 31, range[1] = f ? 127 : 30;
		if (precision) *precision = f ? 23 : 0;
		return;
	}
	BEGIN(OP_glGetShaderPrecisionFormat, 1);
	mgl_w32(st), mgl_w32(pt);
	struct mgl_reader r = mgl_end_sync();
	GLint r0 = mgl_i32(&r), r1 = mgl_i32(&r), p = mgl_i32(&r);
	mgl_unlock();
	if (range) range[0] = r0, range[1] = r1;
	if (precision) *precision = p;
}

API void E glShaderBinary(GLsizei n, const GLuint *s, GLenum fmt, const void *bin, GLsizei len) {
	(void)n, (void)s, (void)fmt, (void)bin, (void)len; /* no binary formats advertised */
}
API void E glReleaseShaderCompiler(void) {}
