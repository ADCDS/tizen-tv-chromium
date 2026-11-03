/* mgl wire protocol, shared by the client (libEGL.so.1 in Chromium's chroot)
 * and the server (TV-native, replays on the Mali). Both ends are little-endian.
 *
 * Client -> server: a stream of messages
 *     struct mgl_hdr { u16 op; u16 flags; u32 size; }  then `size` payload bytes
 * Payload = 32-bit words (ints, enums, floats as bits, client object ids),
 * then variable-length blobs, each `u32 len` + bytes padded to 4.
 * Messages with MGL_F_SYNC make the server answer with
 *     u32 size, then `size` reply bytes
 * and the client blocks until it arrives. Everything else is fire-and-forget,
 * batched client-side and flushed on sync calls, swaps and glFlush/glFinish.
 */
#ifndef MGL_PROTO_H
#define MGL_PROTO_H
#include <stdint.h>
#include <string.h>

#define MGL_SOCKET_ENV "MGL_SOCKET"
#define MGL_SOCKET_DEFAULT "/tmp/mgl.sock"
#define MGL_PROTO_VERSION 1

struct mgl_hdr {
	uint16_t op;
	uint16_t flags;
	uint32_t size;
};
#define MGL_F_SYNC 1

/* Ops not generated from the GL headers. GL ops start at MGL_OP_GL_BASE. */
enum {
	MGL_OP_HELLO = 1,         /* u32 version -> u32 version */
	MGL_OP_MAKE_CURRENT,      /* u32 ctx, u32 draw, u32 read */
	MGL_OP_EGL_INITIALIZE,    /* -> i32 ok, i32 major, i32 minor */
	MGL_OP_EGL_GET_CONFIGS,   /* -> u32 n, then n * (u32 id, i32 attrs[MGL_NCFGATTR]) */
	MGL_OP_EGL_CHOOSE_CONFIG, /* blob attrib_list (i32 pairs, EGL_NONE) -> u32 n, n * u32 id */
	MGL_OP_GL_FINISH_SYNC,    /* -> u32 0: round trip after glFinish */
	MGL_OP_EGL_CREATE_CONTEXT, /* u32 id, u32 cfg, u32 share, i32 client_version -> i32 ok */
	MGL_OP_EGL_DESTROY_CONTEXT, /* u32 id */
	MGL_OP_EGL_CREATE_WINDOW, /* u32 id, u32 cfg, i32 w, i32 h -> i32 ok */
	MGL_OP_EGL_CREATE_PBUFFER, /* u32 id, u32 cfg, i32 w, i32 h -> i32 ok */
	MGL_OP_EGL_DESTROY_SURFACE, /* u32 id */
	MGL_OP_EGL_RESIZE_WINDOW, /* u32 id, i32 w, i32 h */
	MGL_OP_EGL_SWAP,          /* u32 surface -> i32 ok (sync: throttles to the display) */
	MGL_OP_EGL_SWAP_INTERVAL, /* i32 interval */
	MGL_OP_EGL_QUERY_SURFACE, /* u32 surface, i32 attr -> i32 ok, i32 value */
	MGL_OP_EGL_CREATE_SYNC,   /* u32 id, i32 type */
	MGL_OP_EGL_CLIENT_WAIT_SYNC, /* u32 id, i32 flags, u64 timeout -> i32 result */
	MGL_OP_EGL_DESTROY_SYNC,  /* u32 id */
	MGL_OP_GL_BASE = 0x100,
};

/* Config attributes shipped by MGL_OP_EGL_GET_CONFIGS, in this order. */
#define MGL_CFGATTRS \
	X(EGL_BUFFER_SIZE) X(EGL_RED_SIZE) X(EGL_GREEN_SIZE) X(EGL_BLUE_SIZE) X(EGL_ALPHA_SIZE) \
	X(EGL_DEPTH_SIZE) X(EGL_STENCIL_SIZE) X(EGL_SAMPLES) X(EGL_SAMPLE_BUFFERS) \
	X(EGL_SURFACE_TYPE) X(EGL_RENDERABLE_TYPE) X(EGL_CONFORMANT) X(EGL_CONFIG_CAVEAT) \
	X(EGL_NATIVE_RENDERABLE) X(EGL_NATIVE_VISUAL_ID) X(EGL_NATIVE_VISUAL_TYPE) \
	X(EGL_LEVEL) X(EGL_MAX_PBUFFER_WIDTH) X(EGL_MAX_PBUFFER_HEIGHT) X(EGL_MAX_PBUFFER_PIXELS) \
	X(EGL_MIN_SWAP_INTERVAL) X(EGL_MAX_SWAP_INTERVAL) X(EGL_BIND_TO_TEXTURE_RGB) \
	X(EGL_BIND_TO_TEXTURE_RGBA) X(EGL_COLOR_BUFFER_TYPE) X(EGL_LUMINANCE_SIZE) \
	X(EGL_ALPHA_MASK_SIZE) X(EGL_TRANSPARENT_TYPE)

/* Little helpers for building/reading payloads. */
static inline uint32_t mgl_pad4(uint32_t n) { return (n + 3) & ~3u; }

struct mgl_reader {
	const uint8_t *p, *end;
};
static inline uint32_t mgl_u32(struct mgl_reader *r) {
	uint32_t v = 0;
	if (r->p + 4 <= r->end) memcpy(&v, r->p, 4);
	r->p += 4;
	return v;
}
static inline int32_t mgl_i32(struct mgl_reader *r) { return (int32_t)mgl_u32(r); }
static inline float mgl_f32(struct mgl_reader *r) {
	uint32_t u = mgl_u32(r);
	float f;
	memcpy(&f, &u, 4);
	return f;
}
static inline uint64_t mgl_u64(struct mgl_reader *r) {
	uint64_t lo = mgl_u32(r);
	return lo | (uint64_t)mgl_u32(r) << 32;
}
/* Blob: returns pointer into the message (valid until the next message). */
static inline const void *mgl_blob(struct mgl_reader *r, uint32_t *len) {
	uint32_t n = mgl_u32(r);
	const void *p = r->p;
	r->p += mgl_pad4(n);
	if (len) *len = n;
	return n ? p : NULL;
}
#endif
