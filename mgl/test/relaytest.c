/* relaytest: end-to-end check of the mgl relay without Chromium.
 * Draws a textured quad (fragment shader uses highp, as ANGLE's do), reads a
 * pixel back and checks it, then times N frames of draw + swap. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
static GLuint sh(GLenum t, const char *s) {
	GLuint x = glCreateShader(t);
	glShaderSource(x, 1, &s, NULL);
	glCompileShader(x);
	GLint ok = 0;
	glGetShaderiv(x, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(x, sizeof log, NULL, log);
		printf("compile failed: %s\n", log);
		exit(1);
	}
	return x;
}

int main(int argc, char **argv) {
	int W = 1280, H = 720, N = argc > 1 ? atoi(argv[1]) : 200;
	EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint ma, mi, n;
	if (!eglInitialize(d, &ma, &mi)) return printf("eglInitialize failed\n"), 1;
	printf("EGL %d.%d vendor=%s ext=%s\n", ma, mi, eglQueryString(d, EGL_VENDOR), eglQueryString(d, EGL_EXTENSIONS));
	EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
	               EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
	EGLConfig c;
	if (!eglChooseConfig(d, ca, &c, 1, &n) || n < 1) return printf("no config\n"), 1;
	EGLint es3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE}, es2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	printf("ES3 context refused: %s\n", eglCreateContext(d, c, EGL_NO_CONTEXT, es3) == EGL_NO_CONTEXT ? "yes" : "NO");
	EGLContext ctx = eglCreateContext(d, c, EGL_NO_CONTEXT, es2);
	EGLint pa[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
	EGLSurface s = eglCreatePbufferSurface(d, c, pa);
	if (!ctx || !s || !eglMakeCurrent(d, s, s, ctx)) return printf("context/surface failed\n"), 1;
	printf("GL_RENDERER=%s\nGL_VERSION=%s\nGL_EXTENSIONS=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_EXTENSIONS));
	GLint range[2], prec;
	glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &prec);
	printf("fragment highp reported: range %d,%d precision %d\n", range[0], range[1], prec);

	GLuint p = glCreateProgram();
	glAttachShader(p, sh(GL_VERTEX_SHADER, "attribute vec2 a; varying highp vec2 uv; void main(){ uv = a*0.5+0.5; gl_Position = vec4(a,0.,1.); }"));
	glAttachShader(p, sh(GL_FRAGMENT_SHADER, "precision highp float; varying highp vec2 uv; uniform sampler2D t; uniform highp vec4 tint;"
	                                         " void main(){ gl_FragColor = texture2D(t, uv) * tint; }"));
	glBindAttribLocation(p, 0, "a");
	glLinkProgram(p);
	GLint linked = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &linked);
	printf("link: %s\n", linked ? "ok" : "FAILED");
	glUseProgram(p);
	glUniform4f(glGetUniformLocation(p, "tint"), 1.0f, 0.5f, 1.0f, 1.0f);

	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	static const GLfloat q[] = {-1, -1, 1, -1, -1, 1, 1, 1};
	glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);

	int TW = 1280, TH = 720;
	uint32_t *px = malloc((size_t)TW * TH * 4);
	for (int i = 0; i < TW * TH; i++) px[i] = 0xff20c0e0; /* ABGR in memory: r=e0 g=c0 b=20 */
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TW, TH, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glViewport(0, 0, W, H);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	unsigned char rgba[4] = {0};
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	int good = abs(rgba[0] - 0xe0) < 3 && abs(rgba[1] - 0x60) < 3 && abs(rgba[2] - 0x20) < 3;
	printf("readback %02x%02x%02x%02x (expect e06020ff): %s\n", rgba[0], rgba[1], rgba[2], rgba[3], good ? "OK" : "WRONG");
	printf("glGetError=0x%x\n", glGetError());

	double t0 = now();
	for (int f = 0; f < N; f++) {
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		eglSwapBuffers(d, s);
	}
	double t1 = now();
	for (int f = 0; f < N / 4; f++) {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TW, TH, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		eglSwapBuffers(d, s);
	}
	double t2 = now();
	for (int f = 0; f < N; f++) (void)glGetError();
	double t3 = now();
	printf("draw+swap: %.0f fps; with 1280x720 upload: %.1f fps; glGetError round trip: %.0f us\n", N / (t1 - t0), (N / 4) / (t2 - t1),
	       (t3 - t2) / N * 1e6);
	return good ? 0 : 2;
}
