/* mgl client core: transport, batching, sync calls, per-thread current state,
 * client-side object names. Every GL/EGL entry point runs under one lock:
 *     mgl_lock(); mgl_begin(op, sync); mgl_w32(...)...; mgl_end() / mgl_end_sync(); mgl_unlock();
 */
#define _GNU_SOURCE
#include "core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static int fd = -1;
static uint8_t *buf;
static size_t buf_len, buf_cap, msg_start;
static uint8_t *reply;
static size_t reply_cap;
static struct mgl_cur sent = {0, 0, 0}; /* last MakeCurrent the server saw */
static __thread struct mgl_cur cur;

struct mgl_ctx *mgl_ctx_table[MGL_MAX_CTX];
static uint32_t next_ctx = 1, next_surface = 1, next_group = 1, next_sync = 1;
struct mgl_group mgl_groups[MGL_MAX_CTX];

void mgl_die(const char *what) {
	fprintf(stderr, "mgl: fatal: %s (%s)\n", what, strerror(errno));
	abort(); /* Chromium restarts its GPU process */
}

void mgl_lock(void) { pthread_mutex_lock(&lock); }
void mgl_unlock(void) { pthread_mutex_unlock(&lock); }

static void write_all(const void *p, size_t n) {
	const uint8_t *b = p;
	while (n) {
		ssize_t w = write(fd, b, n);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) mgl_die("write to mgl server");
		b += w;
		n -= (size_t)w;
	}
}
static void read_all(void *p, size_t n) {
	uint8_t *b = p;
	while (n) {
		ssize_t r = read(fd, b, n);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) mgl_die("read from mgl server");
		b += r;
		n -= (size_t)r;
	}
}

static void ensure_cap(size_t extra) {
	if (buf_len + extra <= buf_cap) return;
	size_t cap = buf_cap ? buf_cap : 1 << 16;
	while (cap < buf_len + extra) cap *= 2;
	buf = realloc(buf, cap);
	if (!buf) mgl_die("out of memory");
	buf_cap = cap;
}

void mgl_flush(void) {
	if (buf_len) write_all(buf, buf_len);
	buf_len = 0;
}

static void raw_begin(uint16_t op, int sync) {
	ensure_cap(sizeof(struct mgl_hdr));
	msg_start = buf_len;
	struct mgl_hdr h = {op, (uint16_t)(sync ? MGL_F_SYNC : 0), 0};
	memcpy(buf + buf_len, &h, sizeof h);
	buf_len += sizeof h;
}
static void raw_end(void) {
	uint32_t size = (uint32_t)(buf_len - msg_start - sizeof(struct mgl_hdr));
	memcpy(buf + msg_start + offsetof(struct mgl_hdr, size), &size, 4);
}

static struct mgl_reader wait_reply(void) {
	uint32_t n;
	read_all(&n, 4);
	if (n > reply_cap) {
		reply = realloc(reply, n);
		if (!reply) mgl_die("out of memory");
		reply_cap = n;
	}
	read_all(reply, n);
	return (struct mgl_reader){reply, reply + n};
}

static void connect_server(void) {
	if (fd >= 0) return;
	const char *path = getenv(MGL_SOCKET_ENV);
	if (!path) path = MGL_SOCKET_DEFAULT;
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	strncpy(a.sun_path, path, sizeof a.sun_path - 1);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) mgl_die("connect to mgl server");
	raw_begin(MGL_OP_HELLO, 1);
	mgl_w32(MGL_PROTO_VERSION);
	raw_end();
	mgl_flush();
	struct mgl_reader r = wait_reply();
	if (mgl_u32(&r) != MGL_PROTO_VERSION) mgl_die("mgl protocol version mismatch");
}

/* Start a message for the calling thread; prefixes a MakeCurrent if needed. */
void mgl_begin(uint16_t op, int sync) {
	connect_server();
	int needs_ctx = op >= MGL_OP_GL_BASE || op == MGL_OP_EGL_SWAP || op == MGL_OP_EGL_SWAP_INTERVAL ||
	                op == MGL_OP_EGL_CREATE_SYNC || op == MGL_OP_EGL_CLIENT_WAIT_SYNC || op == MGL_OP_GL_FINISH_SYNC;
	if (needs_ctx && memcmp(&cur, &sent, sizeof cur)) {
		raw_begin(MGL_OP_MAKE_CURRENT, 0);
		mgl_w32(cur.ctx);
		mgl_w32(cur.draw);
		mgl_w32(cur.read);
		raw_end();
		sent = cur;
	}
	raw_begin(op, sync);
}
void mgl_end(void) {
	raw_end();
	if (buf_len > (1 << 20)) mgl_flush();
}
struct mgl_reader mgl_end_sync(void) {
	raw_end();
	mgl_flush();
	return wait_reply();
}

void mgl_w32(uint32_t v) {
	ensure_cap(4);
	memcpy(buf + buf_len, &v, 4);
	buf_len += 4;
}
void mgl_wf(float f) {
	uint32_t u;
	memcpy(&u, &f, 4);
	mgl_w32(u);
}
void mgl_w64(uint64_t v) {
	mgl_w32((uint32_t)v);
	mgl_w32((uint32_t)(v >> 32));
}
void mgl_wblob(const void *p, uint32_t len) {
	if (!p) len = 0;
	mgl_w32(len);
	ensure_cap(mgl_pad4(len));
	if (len) memcpy(buf + buf_len, p, len);
	memset(buf + buf_len + len, 0, mgl_pad4(len) - len);
	buf_len += mgl_pad4(len);
}

/* ---- current state and object names ---- */

struct mgl_cur *mgl_current(void) { return &cur; }
void mgl_set_current(uint32_t ctx, uint32_t draw, uint32_t read) {
	cur.ctx = ctx;
	cur.draw = draw;
	cur.read = read;
}
struct mgl_ctx *mgl_ctx_get(uint32_t id) { return id && id < MGL_MAX_CTX ? mgl_ctx_table[id] : NULL; }

uint32_t mgl_new_ctx(uint32_t share) {
	uint32_t id = next_ctx++;
	if (id >= MGL_MAX_CTX) return 0;
	struct mgl_ctx *c = calloc(1, sizeof *c);
	c->id = id;
	struct mgl_ctx *s = mgl_ctx_get(share);
	c->group = s ? s->group : next_group++;
	for (int i = 0; i < MGL_NS_COUNT; i++) c->next[i] = 1;
	mgl_ctx_table[id] = c;
	struct mgl_group *g = &mgl_groups[c->group];
	if (!g->used) {
		g->used = 1;
		for (int i = 0; i < MGL_NS_COUNT; i++) g->next[i] = 1;
	}
	return id;
}
uint32_t mgl_new_surface(void) { return next_surface++; }
uint32_t mgl_new_sync(void) { return next_sync++; }

/* Allocate a client-side object name in the current context's namespace. */
uint32_t mgl_alloc_name(int ns) {
	struct mgl_ctx *c = mgl_ctx_get(cur.ctx);
	if (!c) return 0;
	if (ns == MGL_NS_FRAMEBUFFER || ns == MGL_NS_VERTEX_ARRAY) return c->next[ns]++; /* per context */
	return mgl_groups[c->group].next[ns]++;
}
