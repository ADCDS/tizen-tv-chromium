/* mgl-server: replays GL/EGL calls from the mgl client (Chromium's GPU process)
 * on the local driver. One client at a time; a new connection (Chromium
 * restarted its GPU process) starts from a clean slate.
 * usage: mgl-server [socket-path] [-v]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "server.h"
#include "../gl_ops.h"
#include <dlfcn.h>
#include <ucontext.h>

/* Last ops executed, dumped if the driver crashes. With MGL_RING=path they
 * also live in a mmapped file, which survives a crash in a driver thread
 * (those block signals, so the handler below never runs). */
struct ring_entry { uint16_t op; uint16_t pad; uint32_t size; uint32_t seq; };
static struct ring_entry recent_mem[64], *recent = recent_mem;
static unsigned recent_n;
#include <fcntl.h>
#include <sys/mman.h>
static void ring_open(void) {
	const char *path = getenv("MGL_RING");
	if (!path) return;
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || ftruncate(fd, sizeof recent_mem)) return;
	void *m = mmap(NULL, sizeof recent_mem, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m != MAP_FAILED) recent = m;
	close(fd);
}
static void log_maps(void) {
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	while (f && fgets(line, sizeof line, f))
		if (strstr(line, "r-xp") && strchr(line, '/')) fprintf(stderr, "  map %s", line);
	if (f) fclose(f);
}
static const char *op_name(uint16_t op) {
	static char buf[16];
	if (op >= MGL_OP_GL_BASE && op < MGL_OP_GL_BASE + MGL_GL_NOPS) return mgl_gl_op_names[op - MGL_OP_GL_BASE];
	snprintf(buf, sizeof buf, "op%u", op);
	return buf;
}
static void where(const char *what, void *addr) {
	Dl_info di;
	if (dladdr(addr, &di) && di.dli_fname)
		fprintf(stderr, "  %s %p = %s+0x%lx (%s)\n", what, addr, di.dli_fname, (unsigned long)((char *)addr - (char *)di.dli_fbase),
		        di.dli_sname ? di.dli_sname : "?");
	else
		fprintf(stderr, "  %s %p\n", what, addr);
}
static void on_crash(int sig, siginfo_t *si, void *ctx) {
	fprintf(stderr, "mgl: CRASH signal %d at address %p\n", sig, si->si_addr);
#if defined(__arm__)
	ucontext_t *uc = ctx;
	where("pc", (void *)uc->uc_mcontext.arm_pc);
	where("lr", (void *)uc->uc_mcontext.arm_lr);
#else
	(void)ctx;
#endif
	fprintf(stderr, "  last ops (oldest first):");
	for (unsigned i = 0; i < 64; i++) {
		unsigned k = (recent_n + i) % 64;
		if (recent[k].op) fprintf(stderr, " %s(%u)", op_name(recent[k].op), recent[k].size);
	}
	fprintf(stderr, "\n");
	signal(sig, SIG_DFL);
	raise(sig);
}

#define MAXOBJ 4096

int verbose;
static EGLDisplay dpy;
static EGLConfig configs[512];
static EGLint nconfigs;
static struct sctx *ctxs[MAXOBJ];
static struct ssurface *surfs[MAXOBJ];
static EGLSyncKHR syncs[MAXOBJ];
static EGLConfig ctx_cfg[MAXOBJ];
static EGLSurface ctx_dummy[MAXOBJ]; /* 1x1 pbuffer for surfaceless binds (no KHR_surfaceless) */
struct sctx *cur_ctx;
static unsigned long msgs_since;
static PFNEGLCREATESYNCKHRPROC p_create_sync;
static PFNEGLCLIENTWAITSYNCKHRPROC p_wait_sync;
static PFNEGLDESTROYSYNCKHRPROC p_destroy_sync;

/* ---- name maps ---- */
uint32_t nmap_get(struct nmap *m, uint32_t id) { return id < m->n ? m->v[id] : 0; }
void nmap_set(struct nmap *m, uint32_t id, uint32_t real) {
	if (id >= m->n) {
		uint32_t n = m->n ? m->n : 256;
		while (n <= id) n *= 2;
		m->v = realloc(m->v, n * sizeof *m->v);
		memset(m->v + m->n, 0, (n - m->n) * sizeof *m->v);
		m->n = n;
	}
	m->v[id] = real;
}
static struct nmap *ns_map(int ns) {
	if (!cur_ctx) return NULL;
	if (ns == NS_FRAMEBUFFER) return &cur_ctx->fbo;
	if (ns == NS_VERTEX_ARRAY) return &cur_ctx->vao;
	return &cur_ctx->group->ns[ns];
}
uint32_t create_name(int ns, uint32_t id) {
	struct nmap *m = ns_map(ns);
	if (!m || !id) return 0;
	GLuint r = 0;
	switch (ns) {
	case NS_TEXTURE: glGenTextures(1, &r); break;
	case NS_BUFFER: glGenBuffers(1, &r); break;
	case NS_RENDERBUFFER: glGenRenderbuffers(1, &r); break;
	case NS_FRAMEBUFFER: glGenFramebuffers(1, &r); break;
	case NS_VERTEX_ARRAY: {
		static PFNGLGENVERTEXARRAYSOESPROC gen;
		if (!gen) gen = (PFNGLGENVERTEXARRAYSOESPROC)eglGetProcAddress("glGenVertexArraysOES");
		if (gen) gen(1, &r);
		break;
	}
	default: return 0; /* programs/shaders are created by their own calls */
	}
	nmap_set(m, id, r);
	return r;
}
uint32_t map_name(int ns, uint32_t id) {
	struct nmap *m = ns_map(ns);
	if (!m || !id) return 0;
	uint32_t r = nmap_get(m, id);
	/* ES2 lets apps bind names they never generated; create on first use. */
	return r ? r : create_name(ns, id);
}
void forget_name(int ns, uint32_t id) {
	struct nmap *m = ns_map(ns);
	if (m && id < m->n) m->v[id] = 0;
}
void note_shader_type(uint32_t id, uint32_t type) {
	if (cur_ctx) nmap_set(&cur_ctx->group->shader_type, id, type);
}
uint32_t shader_type(uint32_t id) { return cur_ctx ? nmap_get(&cur_ctx->group->shader_type, id) : 0; }

/* ---- replies ---- */
static uint8_t *rbuf;
static uint32_t rlen, rcap;
void *reply_space(uint32_t n) {
	if (rlen + n > rcap) {
		rcap = rcap ? rcap : 4096;
		while (rlen + n > rcap) rcap *= 2;
		rbuf = realloc(rbuf, rcap);
	}
	void *p = rbuf + rlen;
	rlen += n;
	return p;
}
void reply_u32(uint32_t v) { memcpy(reply_space(4), &v, 4); }
void reply_f32(float v) { memcpy(reply_space(4), &v, 4); }
void reply_blob(const void *p, uint32_t n) {
	reply_u32(n);
	uint8_t *d = reply_space(mgl_pad4(n));
	if (n) memcpy(d, p, n);
	memset(d + n, 0, mgl_pad4(n) - n);
}

/* ---- EGL ops ---- */
static uint32_t cfg_index(EGLConfig c) {
	for (int i = 0; i < nconfigs; i++)
		if (configs[i] == c) return (uint32_t)i + 1;
	return 0;
}
static EGLConfig cfg_get(uint32_t id) { return id && (EGLint)id <= nconfigs ? configs[id - 1] : NULL; }
static struct ssurface *surf_get(uint32_t id) { return id && id < MAXOBJ ? surfs[id] : NULL; }
static struct sctx *ctx_get(uint32_t id) { return id && id < MAXOBJ ? ctxs[id] : NULL; }

static void make_current(uint32_t c, uint32_t d, uint32_t r) {
	struct sctx *ctx = ctx_get(c);
	struct ssurface *ds = surf_get(d), *rs = surf_get(r);
	EGLSurface de = ds ? ds->egl : EGL_NO_SURFACE, re = rs ? rs->egl : EGL_NO_SURFACE;
	if (ctx && de == EGL_NO_SURFACE) {
		if (!ctx_dummy[c]) {
			EGLint a[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
			ctx_dummy[c] = eglCreatePbufferSurface(dpy, ctx_cfg[c], a);
		}
		de = re = ctx_dummy[c];
	}
	int ok = eglMakeCurrent(dpy, de, re, ctx ? ctx->egl : EGL_NO_CONTEXT);
	if (verbose) fprintf(stderr, "mgl: make current ctx %u draw %u read %u%s -> %s 0x%x\n", c, d, r,
	                     de != (ds ? ds->egl : EGL_NO_SURFACE) ? " (dummy pbuffer)" : "", ok ? "ok" : "FAILED", ok ? 0 : eglGetError());
	cur_ctx = ctx;
}

static void reset_session(void) {
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	cur_ctx = NULL;
	for (int i = 0; i < MAXOBJ; i++) {
		if (ctxs[i]) {
			eglDestroyContext(dpy, ctxs[i]->egl);
			free(ctxs[i]->fbo.v);
			free(ctxs[i]->vao.v);
			free(ctxs[i]);
			ctxs[i] = NULL;
		}
		if (ctx_dummy[i]) eglDestroySurface(dpy, ctx_dummy[i]), ctx_dummy[i] = NULL;
		if (surfs[i]) {
			eglDestroySurface(dpy, surfs[i]->egl);
			if (surfs[i]->win) plat_destroy_window(surfs[i]->win);
			free(surfs[i]);
			surfs[i] = NULL;
		}
		if (syncs[i] && p_destroy_sync) p_destroy_sync(dpy, syncs[i]), syncs[i] = NULL;
	}
	/* share groups are leaked on reset; they hold only name maps */
}

static void handle(uint16_t op, struct mgl_reader *r) {
	switch (op) {
	case MGL_OP_HELLO: reply_u32(MGL_PROTO_VERSION); break;
	case MGL_OP_MAKE_CURRENT: {
		uint32_t c = mgl_u32(r), d = mgl_u32(r), rd = mgl_u32(r);
		make_current(c, d, rd);
		break;
	}
	case MGL_OP_EGL_INITIALIZE: reply_u32(1), reply_u32(1), reply_u32(4); break;
	case MGL_OP_EGL_GET_CONFIGS:
		reply_u32((uint32_t)nconfigs);
		for (int i = 0; i < nconfigs; i++) {
			reply_u32((uint32_t)i + 1);
			EGLint v;
#define X(a) v = 0, eglGetConfigAttrib(dpy, configs[i], a, &v), reply_u32((uint32_t)v);
			MGL_CFGATTRS
#undef X
		}
		break;
	case MGL_OP_EGL_CHOOSE_CONFIG: {
		uint32_t len;
		const EGLint *attrs = mgl_blob(r, &len);
		EGLConfig out[512];
		EGLint n = 0;
		if (!eglChooseConfig(dpy, attrs, out, 512, &n)) n = 0;
		if (verbose) {
			fprintf(stderr, "mgl: eglChooseConfig(");
			for (uint32_t i = 0; attrs && i + 1 < len / 4 && attrs[i] != EGL_NONE; i += 2) fprintf(stderr, " 0x%x=0x%x", attrs[i], attrs[i + 1]);
			fprintf(stderr, " ) -> %d:", n);
			for (EGLint i = 0; i < n; i++) fprintf(stderr, " %u", cfg_index(out[i]));
			fprintf(stderr, "\n");
		}
		uint32_t ids[512], k = 0;
		for (EGLint i = 0; i < n; i++)
			if ((ids[k] = cfg_index(out[i]))) k++; /* drop configs we don't offer */
		reply_u32(k);
		for (uint32_t i = 0; i < k; i++) reply_u32(ids[i]);
		break;
	}
	case MGL_OP_GL_FINISH_SYNC: glFinish(); reply_u32(0); break;
	case MGL_OP_EGL_CREATE_CONTEXT: {
		uint32_t id = mgl_u32(r), cfg = mgl_u32(r), share = mgl_u32(r);
		int32_t ver = mgl_i32(r);
		struct sctx *sh = ctx_get(share);
		EGLint a[] = {EGL_CONTEXT_CLIENT_VERSION, ver ? ver : 2, EGL_NONE};
		EGLContext e = id < MAXOBJ ? eglCreateContext(dpy, cfg_get(cfg), sh ? sh->egl : EGL_NO_CONTEXT, a) : EGL_NO_CONTEXT;
		if (verbose) fprintf(stderr, "mgl: create context %u cfg %u share %u -> %s\n", id, cfg, share, e != EGL_NO_CONTEXT ? "ok" : "FAILED");
		if (e != EGL_NO_CONTEXT) {
			struct sctx *c = calloc(1, sizeof *c);
			c->egl = e;
			c->group = sh ? sh->group : calloc(1, sizeof(struct sgroup));
			ctxs[id] = c;
			ctx_cfg[id] = cfg_get(cfg);
		} else if (verbose)
			fprintf(stderr, "mgl: eglCreateContext failed 0x%x\n", eglGetError());
		reply_u32(e != EGL_NO_CONTEXT);
		break;
	}
	case MGL_OP_EGL_DESTROY_CONTEXT: {
		uint32_t id = mgl_u32(r);
		struct sctx *c = ctx_get(id);
		if (c) {
			if (c == cur_ctx) eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), cur_ctx = NULL;
			eglDestroyContext(dpy, c->egl);
			free(c->fbo.v), free(c->vao.v), free(c);
			ctxs[id] = NULL;
		}
		break;
	}
	case MGL_OP_EGL_CREATE_WINDOW:
	case MGL_OP_EGL_CREATE_PBUFFER: {
		uint32_t id = mgl_u32(r), cfg = mgl_u32(r);
		int32_t w = mgl_i32(r), h = mgl_i32(r);
		struct ssurface *s = calloc(1, sizeof *s);
		s->w = w, s->h = h;
		if (op == MGL_OP_EGL_CREATE_WINDOW)
			s->egl = plat_create_window_surface(dpy, cfg_get(cfg), w, h, &s->win);
		else {
			EGLint a[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
			s->egl = eglCreatePbufferSurface(dpy, cfg_get(cfg), a);
		}
		int ok = s->egl != EGL_NO_SURFACE && id < MAXOBJ;
		if (verbose) fprintf(stderr, "mgl: create %s %u: %dx%d cfg %u -> %s\n", op == MGL_OP_EGL_CREATE_WINDOW ? "window" : "pbuffer", id, w, h, cfg, ok ? "ok" : "FAILED");
		if (ok) surfs[id] = s;
		else {
			if (verbose) fprintf(stderr, "mgl: create surface %dx%d failed 0x%x\n", w, h, eglGetError());
			free(s);
		}
		reply_u32(ok);
		break;
	}
	case MGL_OP_EGL_DESTROY_SURFACE: {
		uint32_t id = mgl_u32(r);
		struct ssurface *s = surf_get(id);
		if (s) {
			eglDestroySurface(dpy, s->egl);
			if (s->win) plat_destroy_window(s->win);
			free(s);
			surfs[id] = NULL;
		}
		break;
	}
	case MGL_OP_EGL_RESIZE_WINDOW: {
		struct ssurface *s = surf_get(mgl_u32(r));
		int32_t w = mgl_i32(r), h = mgl_i32(r);
		if (s && s->win) plat_resize_window(s->win, w, h), s->w = w, s->h = h;
		break;
	}
	case MGL_OP_EGL_SWAP: {
		struct ssurface *s = surf_get(mgl_u32(r));
		int ok = 0;
		if (s) {
			plat_before_swap(s);
			ok = eglSwapBuffers(dpy, s->egl);
			if (verbose && !ok) fprintf(stderr, "mgl: eglSwapBuffers FAILED 0x%x\n", eglGetError());
			plat_after_swap(s);
		}
		{ /* progress: frames and messages per second */
			static unsigned long swaps;
			static struct timespec t0;
			struct timespec t;
			clock_gettime(CLOCK_MONOTONIC, &t);
			if (!swaps++) t0 = t;
			if (swaps % 60 == 0) {
				double dt = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
				fprintf(stderr, "mgl: %lu swaps, %.1f fps, %.0f msgs/s\n", swaps, 60 / dt, msgs_since / dt);
				t0 = t, msgs_since = 0;
			}
		}
		reply_u32(ok);
		break;
	}
	case MGL_OP_EGL_SWAP_INTERVAL: eglSwapInterval(dpy, mgl_i32(r)); break;
	case MGL_OP_EGL_QUERY_SURFACE: {
		struct ssurface *s = surf_get(mgl_u32(r));
		EGLint attr = mgl_i32(r), v = 0;
		int ok = s && eglQuerySurface(dpy, s->egl, attr, &v);
		reply_u32(ok), reply_u32((uint32_t)v);
		break;
	}
	/* Fences: the Mali's own eglClientWaitSyncKHR can block forever under
	 * Chromium's usage, so emulate them. Creating one flushes; waiting on one
	 * finishes all prior work (a superset of what the fence covers). */
	case MGL_OP_EGL_CREATE_SYNC:
		mgl_u32(r), mgl_i32(r);
		glFlush();
		break;
	case MGL_OP_EGL_CLIENT_WAIT_SYNC: {
		mgl_u32(r), mgl_i32(r), mgl_u64(r);
		struct timespec a, b;
		clock_gettime(CLOCK_MONOTONIC, &a);
		/* All GL runs in order on this one thread, and the client copies every
		 * buffer at call time, so a fence only has to flush. MGL_FENCE_FINISH=1
		 * restores the full wait (needed only for cross-context sharing). */
		static int finish = -1;
		if (finish < 0) finish = getenv("MGL_FENCE_FINISH") != NULL;
		if (finish) glFinish();
		else glFlush();
		clock_gettime(CLOCK_MONOTONIC, &b);
		if (verbose > 1) fprintf(stderr, "mgl: fence wait done in %.1f ms\n", (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6);
		reply_u32(EGL_CONDITION_SATISFIED_KHR);
		break;
	}
	case MGL_OP_EGL_DESTROY_SYNC: mgl_u32(r); break;
	default:
		if (op < MGL_OP_GL_BASE || !dispatch_gl(op, r))
			fprintf(stderr, "mgl: unknown op %u\n", op);
	}
}

static int read_all(int fd, void *p, size_t n) {
	uint8_t *b = p;
	while (n) {
		ssize_t k = read(fd, b, n);
		if (k < 0 && errno == EINTR) continue;
		if (k <= 0) return -1;
		b += k, n -= (size_t)k;
	}
	return 0;
}
static int write_all(int fd, const void *p, size_t n) {
	const uint8_t *b = p;
	while (n) {
		ssize_t k = write(fd, b, n);
		if (k < 0 && errno == EINTR) continue;
		if (k <= 0) return -1;
		b += k, n -= (size_t)k;
	}
	return 0;
}

/* Serve one client until it disconnects or a new one connects (Chromium
 * replaced its GPU process, e.g. after a watchdog timeout): returns the new
 * client's fd, or -1. */
static int serve(int lfd, int cfd) {
	static uint8_t *msg;
	static uint32_t cap;
	unsigned long nmsg = 0;
	int next = -1;
	for (;;) {
		struct pollfd pf[2] = {{cfd, POLLIN, 0}, {lfd, POLLIN, 0}};
		if (poll(pf, 2, -1) < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (pf[1].revents & POLLIN) {
			next = accept(lfd, NULL, NULL);
			if (next >= 0) {
				if (verbose) fprintf(stderr, "mgl: new client replaces the current one\n");
				break;
			}
		}
		if (!(pf[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
		struct mgl_hdr h;
		if (read_all(cfd, &h, sizeof h)) break;
		if (h.size > cap) {
			cap = h.size;
			msg = realloc(msg, cap);
		}
		if (h.size && read_all(cfd, msg, h.size)) break;
		struct mgl_reader r = {msg, msg + h.size};
		recent[recent_n % 64].op = h.op, recent[recent_n % 64].size = h.size, recent[recent_n % 64].seq = recent_n, recent_n++;
		{ /* MGL_TRACE=<file>: log every call with its first argument words */
			static FILE *tf;
			static int topen;
			if (!topen) {
				topen = 1;
				const char *tp = getenv("MGL_TRACE");
				if (tp && (tf = fopen(tp, "w"))) setvbuf(tf, NULL, _IOLBF, 0);
			}
			if (tf) {
				const uint32_t *a = (const uint32_t *)msg;
				fprintf(tf, "%s[%u](", op_name(h.op), h.size);
				for (uint32_t k = 0; k < h.size / 4 && k < 12; k++) fprintf(tf, "%s0x%x", k ? "," : "", a[k]);
				fprintf(tf, ")\n");
			}
		}
		rlen = 0;
		handle(h.op, &r);
		{ /* MGL_CHECK=1: log every GL call that raises an error, with its first args */
			static int check = -1, logged;
			if (check < 0) check = getenv("MGL_CHECK") != NULL;
			if (check && h.op >= MGL_OP_GL_BASE && h.op != OP_glGetError && logged < 300) {
				GLenum e = glGetError();
				if (e) {
					const uint32_t *a = (const uint32_t *)msg;
					fprintf(stderr, "mgl: GL error 0x%x after %s(", e, op_name(h.op));
					for (uint32_t k = 0; k < h.size / 4 && k < 10; k++) fprintf(stderr, "%s0x%x", k ? "," : "", a[k]);
					fprintf(stderr, ")\n");
					logged++;
				}
			}
		}
		nmsg++, msgs_since++;
		if (h.flags & MGL_F_SYNC) {
			if (write_all(cfd, &rlen, 4) || (rlen && write_all(cfd, rbuf, rlen))) break;
		}
	}
	if (verbose) fprintf(stderr, "mgl: client gone after %lu messages\n", nmsg);
	return next;
}

int main(int argc, char **argv) {
	const char *path = MGL_SOCKET_DEFAULT;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) verbose = 1;
		else path = argv[i];
	}
	signal(SIGPIPE, SIG_IGN);
	struct sigaction sa = {.sa_sigaction = on_crash, .sa_flags = SA_SIGINFO};
	sigaction(SIGSEGV, &sa, NULL), sigaction(SIGBUS, &sa, NULL), sigaction(SIGILL, &sa, NULL), sigaction(SIGABRT, &sa, NULL);
	ring_open();
	dpy = plat_init();
	if (dpy == EGL_NO_DISPLAY) return 1;
	eglBindAPI(EGL_OPENGL_ES_API);
	/* Only offer configs the client can actually use: ES2-renderable and
	 * pbuffer-capable (the Mali lists e.g. an RGBA8888/d24s8 ES2 config with no
	 * surface types at all, which ANGLE would happily pick and then fail on). */
	{
		EGLConfig all[512];
		EGLint n = 0;
		eglGetConfigs(dpy, all, 512, &n);
		for (EGLint i = 0; i < n; i++) {
			EGLint rend = 0, surf = 0;
			eglGetConfigAttrib(dpy, all[i], EGL_RENDERABLE_TYPE, &rend);
			eglGetConfigAttrib(dpy, all[i], EGL_SURFACE_TYPE, &surf);
			if ((rend & EGL_OPENGL_ES2_BIT) && (surf & EGL_PBUFFER_BIT)) configs[nconfigs++] = all[i];
		}
	}
	p_create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
	p_wait_sync = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
	p_destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
	fprintf(stderr, "mgl-server: %s, %d configs, GL via %s\n", eglQueryString(dpy, EGL_VENDOR), nconfigs, path);
	for (int i = 0; verbose && i < nconfigs; i++) {
		EGLint r = 0, g = 0, b = 0, a = 0, d = 0, st = 0, ms = 0, surf = 0, rend = 0, vis = 0, cav = 0;
		eglGetConfigAttrib(dpy, configs[i], EGL_RED_SIZE, &r), eglGetConfigAttrib(dpy, configs[i], EGL_GREEN_SIZE, &g);
		eglGetConfigAttrib(dpy, configs[i], EGL_BLUE_SIZE, &b), eglGetConfigAttrib(dpy, configs[i], EGL_ALPHA_SIZE, &a);
		eglGetConfigAttrib(dpy, configs[i], EGL_DEPTH_SIZE, &d), eglGetConfigAttrib(dpy, configs[i], EGL_STENCIL_SIZE, &st);
		eglGetConfigAttrib(dpy, configs[i], EGL_SAMPLES, &ms), eglGetConfigAttrib(dpy, configs[i], EGL_SURFACE_TYPE, &surf);
		eglGetConfigAttrib(dpy, configs[i], EGL_RENDERABLE_TYPE, &rend), eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &vis);
		eglGetConfigAttrib(dpy, configs[i], EGL_CONFIG_CAVEAT, &cav);
		fprintf(stderr, "  cfg %2d: rgba %d%d%d%d d%-2d s%d ms%d surf 0x%x%s%s rend 0x%x vis 0x%x caveat 0x%x\n", i + 1, r, g, b, a, d, st, ms,
		        surf, surf & EGL_PBUFFER_BIT ? " pbuf" : "", surf & EGL_WINDOW_BIT ? " win" : "", rend, vis, cav);
	}

	int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	strncpy(a.sun_path, path, sizeof a.sun_path - 1);
	unlink(path);
	if (bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 4)) {
		perror("mgl-server: bind/listen");
		return 1;
	}
	chmod(path, 0777);
	int cfd = -1;
	for (;;) {
		if (cfd < 0) cfd = accept(lfd, NULL, NULL);
		if (cfd < 0) continue;
		if (verbose) fprintf(stderr, "mgl: client connected\n"), log_maps();
		int next = serve(lfd, cfd);
		close(cfd);
		reset_session();
		cfd = next;
	}
}
