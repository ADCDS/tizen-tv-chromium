/* mgl server: decoders for the hand-written GL entry points. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "../gl_ops.h"
#include "server.h"

#define D(name) void s_##name(struct mgl_reader *r)

/* Extension entry points, resolved once per call site (keyed by the literal). */
void *getproc(const char *name) {
	static struct { const char *n; void *f; } cache[64];
	for (int i = 0; i < 64 && cache[i].n; i++)
		if (cache[i].n == name) return cache[i].f;
	void *f = (void *)eglGetProcAddress(name);
	for (int i = 0; i < 64; i++)
		if (!cache[i].n) {
			cache[i].n = name, cache[i].f = f;
			break;
		}
	return f;
}

static uint32_t reverse_name(int ns, uint32_t real) {
	if (!real || !cur_ctx) return 0;
	struct nmap *m = ns == NS_FRAMEBUFFER ? &cur_ctx->fbo : ns == NS_VERTEX_ARRAY ? &cur_ctx->vao : &cur_ctx->group->ns[ns];
	for (uint32_t i = 1; i < m->n; i++)
		if (m->v[i] == real) return i;
	return 0;
}

D(glPixelStorei) {
	GLenum p = mgl_u32(r);
	GLint v = mgl_i32(r);
	glPixelStorei(p, v);
}
D(glTexImage2D) {
	GLenum t = mgl_u32(r);
	GLint level = mgl_i32(r), ifmt = mgl_i32(r);
	GLsizei w = mgl_i32(r), h = mgl_i32(r);
	GLint border = mgl_i32(r);
	GLenum format = mgl_u32(r), type = mgl_u32(r);
	const void *px = mgl_blob(r, NULL);
	glTexImage2D(t, level, ifmt, w, h, border, format, type, px);
}
D(glTexSubImage2D) {
	GLenum t = mgl_u32(r);
	GLint level = mgl_i32(r), x = mgl_i32(r), y = mgl_i32(r);
	GLsizei w = mgl_i32(r), h = mgl_i32(r);
	GLenum format = mgl_u32(r), type = mgl_u32(r);
	const void *px = mgl_blob(r, NULL);
	glTexSubImage2D(t, level, x, y, w, h, format, type, px);
}
D(glCompressedTexImage2D) {
	GLenum t = mgl_u32(r);
	GLint level = mgl_i32(r);
	GLenum ifmt = mgl_u32(r);
	GLsizei w = mgl_i32(r), h = mgl_i32(r);
	GLint border = mgl_i32(r);
	uint32_t n;
	const void *d = mgl_blob(r, &n);
	glCompressedTexImage2D(t, level, ifmt, w, h, border, (GLsizei)n, d);
}
D(glCompressedTexSubImage2D) {
	GLenum t = mgl_u32(r);
	GLint level = mgl_i32(r), x = mgl_i32(r), y = mgl_i32(r);
	GLsizei w = mgl_i32(r), h = mgl_i32(r);
	GLenum format = mgl_u32(r);
	uint32_t n;
	const void *d = mgl_blob(r, &n);
	glCompressedTexSubImage2D(t, level, x, y, w, h, format, (GLsizei)n, d);
}
D(glReadPixels) {
	GLint x = mgl_i32(r), y = mgl_i32(r);
	GLsizei w = mgl_i32(r), h = mgl_i32(r);
	GLenum format = mgl_u32(r), type = mgl_u32(r);
	uint32_t n = mgl_u32(r);
	reply_u32(n);
	void *dst = reply_space(mgl_pad4(n));
	glReadPixels(x, y, w, h, format, type, dst);
}
D(glBufferData) {
	GLenum t = mgl_u32(r);
	GLsizeiptr size = (GLsizeiptr)mgl_u64(r);
	GLenum usage = mgl_u32(r);
	uint32_t has = mgl_u32(r);
	const void *d = mgl_blob(r, NULL);
	glBufferData(t, size, has ? d : NULL, usage);
}
D(glBufferSubData) {
	GLenum t = mgl_u32(r);
	GLintptr off = (GLintptr)mgl_u64(r);
	uint32_t n;
	const void *d = mgl_blob(r, &n);
	glBufferSubData(t, off, (GLsizeiptr)n, d);
}
D(glVertexAttribPointer) {
	GLuint i = mgl_u32(r);
	GLint size = mgl_i32(r);
	GLenum type = mgl_u32(r);
	GLboolean norm = (GLboolean)mgl_u32(r);
	GLsizei stride = mgl_i32(r);
	uintptr_t off = (uintptr_t)mgl_u64(r);
	glVertexAttribPointer(i, size, type, norm, stride, (const void *)off);
}
/* Client-memory attributes shipped with a draw: point them at the copies for
 * this draw (ES2 client arrays are consumed during the call). */
static void client_attribs(struct mgl_reader *r) {
	uint32_t n = mgl_u32(r);
	if (!n) return;
	GLint ab = 0;
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
	if (ab) glBindBuffer(GL_ARRAY_BUFFER, 0);
	for (uint32_t i = 0; i < n; i++) {
		GLuint idx = mgl_u32(r);
		GLint size = mgl_i32(r);
		GLenum type = mgl_u32(r);
		GLboolean norm = (GLboolean)mgl_u32(r);
		GLsizei stride = mgl_i32(r);
		const void *data = mgl_blob(r, NULL);
		glVertexAttribPointer(idx, size, type, norm, stride, data);
	}
	if (ab) glBindBuffer(GL_ARRAY_BUFFER, (GLuint)ab);
}
D(glDrawArrays) {
	GLenum mode = mgl_u32(r);
	GLint first = mgl_i32(r);
	GLsizei count = mgl_i32(r);
	client_attribs(r);
	glDrawArrays(mode, first, count);
}
D(glDrawElements) {
	GLenum mode = mgl_u32(r);
	GLsizei n = mgl_i32(r);
	GLenum type = mgl_u32(r);
	uint32_t client_indices = mgl_u32(r);
	const void *idx = client_indices ? mgl_blob(r, NULL) : (const void *)(uintptr_t)mgl_u64(r);
	client_attribs(r);
	glDrawElements(mode, n, type, idx);
}

/* The Mali-450 has no fragment highp; compile it as mediump. */
static char *demote_highp(const char *src) {
	size_t n = strlen(src);
	char *out = malloc(n * 2 + 1), *o = out;
	for (const char *p = src; *p;) {
		if (!strncmp(p, "highp", 5) && (p == src || !(p[-1] == '_' || (p[-1] | 32) - 'a' < 26u || (unsigned)(p[-1] - '0') < 10u)) &&
		    !(p[5] == '_' || ((p[5] | 32) - 'a') < 26u || (unsigned)(p[5] - '0') < 10u)) {
			memcpy(o, "mediump", 7), o += 7, p += 5;
		} else
			*o++ = *p++;
	}
	*o = 0;
	return out;
}
D(glShaderSource) {
	uint32_t id = mgl_u32(r);
	const char *src = mgl_blob(r, NULL);
	if (!src) src = "";
	/* Fragment shaders: the Mali has no highp. Vertex shaders too, because a
	 * uniform shared by both stages must have the same precision to link; the
	 * Mali-400 vertex processor is FP32 regardless of the qualifier. */
	char *fixed = demote_highp(src);
	const char *s = fixed ? fixed : src;
	glShaderSource(map_name(NS_PROGRAM, id), 1, &s, NULL);
	if (verbose > 1 && fixed) fprintf(stderr, "mgl: fragment shader %u:\n%s\n", id, fixed);
	free(fixed);
}
D(glBindAttribLocation) {
	GLuint p = map_name(NS_PROGRAM, mgl_u32(r));
	GLuint i = mgl_u32(r);
	glBindAttribLocation(p, i, mgl_blob(r, NULL));
}
D(glGetAttribLocation) {
	GLuint p = map_name(NS_PROGRAM, mgl_u32(r));
	reply_u32((uint32_t)glGetAttribLocation(p, mgl_blob(r, NULL)));
}
D(glGetUniformLocation) {
	GLuint p = map_name(NS_PROGRAM, mgl_u32(r));
	reply_u32((uint32_t)glGetUniformLocation(p, mgl_blob(r, NULL)));
}
D(glCreateProgram) {
	uint32_t id = mgl_u32(r);
	if (cur_ctx) nmap_set(&cur_ctx->group->ns[NS_PROGRAM], id, glCreateProgram());
}
D(glCreateShader) {
	uint32_t id = mgl_u32(r);
	GLenum type = mgl_u32(r);
	if (cur_ctx) nmap_set(&cur_ctx->group->ns[NS_PROGRAM], id, glCreateShader(type));
	note_shader_type(id, type);
}
D(glFlush) {
	(void)r;
	glFlush();
}
D(glFinish) {
	(void)r;
	glFinish();
	reply_u32(0);
}
D(glGetError) {
	(void)r;
	reply_u32(glGetError());
}

/* ---- queries: reply u32 count, then values ---- */
static int pname_count(GLenum p) {
	GLint n = 0;
	switch (p) {
	case GL_ALIASED_LINE_WIDTH_RANGE:
	case GL_ALIASED_POINT_SIZE_RANGE:
	case GL_DEPTH_RANGE:
	case GL_MAX_VIEWPORT_DIMS: return 2;
	case GL_BLEND_COLOR:
	case GL_COLOR_CLEAR_VALUE:
	case GL_COLOR_WRITEMASK:
	case GL_SCISSOR_BOX:
	case GL_VIEWPORT: return 4;
	case GL_COMPRESSED_TEXTURE_FORMATS: glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n); return n;
	case GL_SHADER_BINARY_FORMATS: glGetIntegerv(GL_NUM_SHADER_BINARY_FORMATS, &n); return n;
	}
	return 1;
}
static int binding_ns(GLenum p) {
	switch (p) {
	case GL_ARRAY_BUFFER_BINDING:
	case GL_ELEMENT_ARRAY_BUFFER_BINDING: return NS_BUFFER;
	case GL_TEXTURE_BINDING_2D:
	case GL_TEXTURE_BINDING_CUBE_MAP:
	case GL_TEXTURE_BINDING_EXTERNAL_OES: return NS_TEXTURE;
	case GL_FRAMEBUFFER_BINDING: return NS_FRAMEBUFFER;
	case GL_RENDERBUFFER_BINDING: return NS_RENDERBUFFER;
	case GL_CURRENT_PROGRAM: return NS_PROGRAM;
	case GL_VERTEX_ARRAY_BINDING_OES: return NS_VERTEX_ARRAY;
	}
	return -1;
}
D(glGetIntegerv) {
	GLenum p = mgl_u32(r);
	int n = pname_count(p);
	GLint v[256] = {0};
	if (n > 256) n = 256;
	glGetIntegerv(p, v);
	/* The Mali-450 has no vertex texture units, and ANGLE's link check then
	 * rejects every Skia program (it attributes the fragment sampler to the
	 * vertex stage too). Report a few; real vertex-stage sampling would still
	 * fail in the Mali compiler and show up in check_compile/check_link. */
	if (p == GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS && v[0] == 0) v[0] = 4;
	int ns = binding_ns(p);
	if (ns >= 0) v[0] = (GLint)reverse_name(ns, (uint32_t)v[0]);
	reply_u32((uint32_t)n);
	for (int i = 0; i < n; i++) reply_u32((uint32_t)v[i]);
}
D(glGetFloatv) {
	GLenum p = mgl_u32(r);
	int n = pname_count(p);
	GLfloat v[256] = {0};
	if (n > 256) n = 256;
	glGetFloatv(p, v);
	reply_u32((uint32_t)n);
	for (int i = 0; i < n; i++) reply_f32(v[i]);
}
D(glGetBooleanv) { s_glGetIntegerv(r); } /* client sends booleans as integer queries */
#define ONE_INT(name, call) \
	D(name) { \
		uint32_t a = mgl_u32(r), b = mgl_u32(r), c = mgl_u32(r); \
		(void)a, (void)b, (void)c; \
		GLint v = 0; \
		call; \
		reply_u32(1), reply_u32((uint32_t)v); \
	}
ONE_INT(glGetBufferParameteriv, glGetBufferParameteriv(a, b, &v))
ONE_INT(glGetRenderbufferParameteriv, glGetRenderbufferParameteriv(a, b, &v))
ONE_INT(glGetTexParameteriv, glGetTexParameteriv(a, b, &v))
ONE_INT(glGetShaderiv, glGetShaderiv(map_name(NS_PROGRAM, a), b, &v))
ONE_INT(glGetProgramiv, glGetProgramiv(map_name(NS_PROGRAM, a), b, &v))
D(glGetTexParameterfv) {
	GLenum t = mgl_u32(r), p = mgl_u32(r);
	mgl_u32(r);
	GLfloat v = 0;
	glGetTexParameterfv(t, p, &v);
	reply_u32(1), reply_f32(v);
}
D(glGetFramebufferAttachmentParameteriv) {
	GLenum t = mgl_u32(r), a = mgl_u32(r), p = mgl_u32(r);
	GLint v = 0, type = 0;
	glGetFramebufferAttachmentParameteriv(t, a, p, &v);
	if (p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) {
		glGetFramebufferAttachmentParameteriv(t, a, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
		v = (GLint)reverse_name(type == GL_TEXTURE ? NS_TEXTURE : NS_RENDERBUFFER, (uint32_t)v);
	}
	reply_u32(1), reply_u32((uint32_t)v);
}
D(glGetVertexAttribiv) {
	GLuint i = mgl_u32(r);
	GLenum p = mgl_u32(r);
	mgl_u32(r);
	GLint v[4] = {0};
	int n = p == GL_CURRENT_VERTEX_ATTRIB ? 4 : 1;
	glGetVertexAttribiv(i, p, v);
	if (p == GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING) v[0] = (GLint)reverse_name(NS_BUFFER, (uint32_t)v[0]);
	reply_u32((uint32_t)n);
	for (int k = 0; k < n; k++) reply_u32((uint32_t)v[k]);
}
D(glGetVertexAttribfv) {
	GLuint i = mgl_u32(r);
	GLenum p = mgl_u32(r);
	mgl_u32(r);
	GLfloat v[4] = {0};
	int n = p == GL_CURRENT_VERTEX_ATTRIB ? 4 : 1;
	glGetVertexAttribfv(i, p, v);
	reply_u32((uint32_t)n);
	for (int k = 0; k < n; k++) reply_f32(v[k]);
}
D(glGetVertexAttribPointerv) {
	GLuint i = mgl_u32(r);
	GLenum p = mgl_u32(r);
	mgl_u32(r);
	void *ptr = NULL;
	glGetVertexAttribPointerv(i, p, &ptr);
	reply_u32(1), reply_u32((uint32_t)(uintptr_t)ptr);
}
static int uniform_components(GLuint prog, GLint loc) {
	GLint n = 0;
	glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
	for (GLint i = 0; i < n; i++) {
		char name[256];
		GLint size;
		GLenum type;
		glGetActiveUniform(prog, (GLuint)i, sizeof name, NULL, &size, &type, name);
		if (glGetUniformLocation(prog, name) != loc) continue;
		switch (type) {
		case GL_FLOAT_VEC2: case GL_INT_VEC2: case GL_BOOL_VEC2: return 2;
		case GL_FLOAT_VEC3: case GL_INT_VEC3: case GL_BOOL_VEC3: return 3;
		case GL_FLOAT_VEC4: case GL_INT_VEC4: case GL_BOOL_VEC4: case GL_FLOAT_MAT2: return 4;
		case GL_FLOAT_MAT3: return 9;
		case GL_FLOAT_MAT4: return 16;
		default: return 1;
		}
	}
	return 1;
}
D(glGetUniformfv) {
	GLuint p = map_name(NS_PROGRAM, mgl_u32(r));
	GLint loc = mgl_i32(r);
	mgl_u32(r);
	GLfloat v[16] = {0};
	int n = uniform_components(p, loc);
	glGetUniformfv(p, loc, v);
	reply_u32((uint32_t)n);
	for (int k = 0; k < n; k++) reply_f32(v[k]);
}
D(glGetUniformiv) {
	GLuint p = map_name(NS_PROGRAM, mgl_u32(r));
	GLint loc = mgl_i32(r);
	mgl_u32(r);
	GLint v[16] = {0};
	int n = uniform_components(p, loc);
	glGetUniformiv(p, loc, v);
	reply_u32((uint32_t)n);
	for (int k = 0; k < n; k++) reply_u32((uint32_t)v[k]);
}
D(glGetAttachedShaders) {
	GLuint p = map_name(NS_PROGRAM, mgl_u32(r));
	mgl_u32(r), mgl_u32(r);
	GLuint s[64];
	GLsizei n = 0;
	glGetAttachedShaders(p, 64, &n, s);
	reply_u32((uint32_t)n);
	for (GLsizei i = 0; i < n; i++) reply_u32(reverse_name(NS_PROGRAM, s[i]));
}

/* ---- strings ---- */
static void reply_log(GLuint obj, int which) {
	GLint len = 0;
	if (which == 0) glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &len);
	if (which == 1) glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &len);
	if (which == 2) glGetShaderiv(obj, GL_SHADER_SOURCE_LENGTH, &len);
	char *buf = calloc(1, (size_t)len + 1);
	GLsizei got = 0;
	if (len > 0) {
		if (which == 0) glGetShaderInfoLog(obj, len, &got, buf);
		if (which == 1) glGetProgramInfoLog(obj, len, &got, buf);
		if (which == 2) glGetShaderSource(obj, len, &got, buf);
	}
	reply_blob(buf, (uint32_t)got);
	free(buf);
}
D(glGetShaderInfoLog) {
	uint32_t id = mgl_u32(r);
	mgl_u32(r), mgl_u32(r);
	reply_log(map_name(NS_PROGRAM, id), 0);
}
D(glGetProgramInfoLog) {
	uint32_t id = mgl_u32(r);
	mgl_u32(r), mgl_u32(r);
	reply_log(map_name(NS_PROGRAM, id), 1);
}
D(glGetShaderSource) {
	uint32_t id = mgl_u32(r);
	mgl_u32(r), mgl_u32(r);
	reply_log(map_name(NS_PROGRAM, id), 2);
}
static void reply_active(GLuint p, GLuint i, int uniform) {
	char name[512];
	GLsizei len = 0;
	GLint size = 0;
	GLenum type = 0;
	if (uniform) glGetActiveUniform(p, i, sizeof name, &len, &size, &type, name);
	else glGetActiveAttrib(p, i, sizeof name, &len, &size, &type, name);
	reply_u32((uint32_t)size), reply_u32(type), reply_blob(name, (uint32_t)len);
}
D(glGetActiveAttrib) {
	GLuint p = map_name(NS_PROGRAM, mgl_u32(r));
	GLuint i = mgl_u32(r);
	mgl_u32(r);
	reply_active(p, i, 0);
}
D(glGetActiveUniform) {
	GLuint p = map_name(NS_PROGRAM, mgl_u32(r));
	GLuint i = mgl_u32(r);
	mgl_u32(r);
	reply_active(p, i, 1);
}
D(glGetString) {
	const char *s = (const char *)glGetString(mgl_u32(r));
	reply_blob(s, s ? (uint32_t)strlen(s) : 0);
}
D(glGetShaderPrecisionFormat) {
	GLenum st = mgl_u32(r), pt = mgl_u32(r);
	GLint range[2] = {0, 0}, prec = 0;
	glGetShaderPrecisionFormat(st, pt, range, &prec);
	reply_u32((uint32_t)range[0]), reply_u32((uint32_t)range[1]), reply_u32((uint32_t)prec);
}
D(glShaderBinary) { (void)r; }
D(glReleaseShaderCompiler) { (void)r; }

/* Log shader compile failures with the source (verbose), to see what the
 * Mali-450 compiler rejects in ANGLE/Skia output. */
void check_compile(GLuint shader) {
	if (!verbose) return;
	GLint ok = 0, type = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok) return;
	glGetShaderiv(shader, GL_SHADER_TYPE, &type);
	char log[2048] = {0};
	glGetShaderInfoLog(shader, sizeof log, NULL, log);
	fprintf(stderr, "mgl: %s shader compile FAILED: %s\n", type == GL_FRAGMENT_SHADER ? "fragment" : "vertex", log);
	GLint len = 0;
	glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &len);
	char *src = calloc(1, (size_t)len + 1);
	glGetShaderSource(shader, len, NULL, src);
	int line = 1;
	fprintf(stderr, "%4d: ", line);
	for (char *c = src; *c; c++) {
		fputc(*c, stderr);
		if (*c == '\n') fprintf(stderr, "%4d: ", ++line);
	}
	fprintf(stderr, "\n");
	free(src);
}

void check_link(GLuint program) {
	if (!verbose) return;
	GLint ok = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (ok) return;
	char log[2048] = {0};
	glGetProgramInfoLog(program, sizeof log, NULL, log);
	fprintf(stderr, "mgl: program link FAILED: %s\n", log);
}
