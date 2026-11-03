"""Generate the mechanical part of the GL remoting layer from the Khronos headers.

Outputs (in OUTDIR):
  gl_ops.h          opcode per function (shared)
  client_gl_gen.c   encoders  (client, exported GL symbols + lookup table)
  server_gl_gen.c   decoders  (server, dispatch_gl)
Functions listed in SPECIAL are hand-written in client/gl_special.c and
server/gl_special.c; the generator only assigns them opcodes.

usage: gen_remote.py OUTDIR gl2.h gl2ext.h
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parse import parse  # noqa: E402

OUT = sys.argv[1]
funcs = {f["name"]: f for f in parse(sys.argv[2:])}

# Extensions exposed to ANGLE besides core ES 2.0 (all present on the Mali-450).
EXT_FUNCS = [
    "glGenVertexArraysOES", "glDeleteVertexArraysOES", "glBindVertexArrayOES", "glIsVertexArrayOES",
    "glDiscardFramebufferEXT",
    "glRenderbufferStorageMultisampleEXT", "glFramebufferTexture2DMultisampleEXT",
]
CORE = [n for n, f in funcs.items() if re.match(r"gl[A-Z]", n) and not re.search(r"(OES|EXT|KHR|ARB|NV|ANGLE|APPLE|ARM|IMG|QCOM|AMD|INTEL|MESA|OVR|DMP|FJ|VIV|SEC|CHROMIUM|WEBGL)$", n)]
# The ES 2.0 core header lists only ES2 functions; gl2ext adds suffixed ones.
NAMES = sorted(set(CORE) | set(EXT_FUNCS))

SPECIAL = {
    # strings / sources
    "glShaderSource", "glBindAttribLocation", "glGetAttribLocation", "glGetUniformLocation",
    # pixel / buffer data
    "glBufferData", "glBufferSubData", "glTexImage2D", "glTexSubImage2D",
    "glCompressedTexImage2D", "glCompressedTexSubImage2D", "glReadPixels", "glPixelStorei",
    # pointers that are buffer offsets
    "glVertexAttribPointer", "glDrawElements", "glDrawArrays",
    # queries with output arrays / strings
    "glGetString", "glGetShaderPrecisionFormat", "glGetBooleanv", "glGetFloatv", "glGetIntegerv",
    "glGetTexParameterfv", "glGetTexParameteriv", "glGetBufferParameteriv",
    "glGetFramebufferAttachmentParameteriv", "glGetRenderbufferParameteriv",
    "glGetShaderiv", "glGetProgramiv", "glGetShaderInfoLog", "glGetProgramInfoLog",
    "glGetShaderSource", "glGetActiveAttrib", "glGetActiveUniform", "glGetAttachedShaders",
    "glGetUniformfv", "glGetUniformiv", "glGetVertexAttribfv", "glGetVertexAttribiv",
    "glGetVertexAttribPointerv", "glShaderBinary", "glReleaseShaderCompiler",
    # object creation / sync
    "glCreateProgram", "glCreateShader", "glFinish", "glFlush", "glGetError",
}

SCALAR = {"GLenum", "GLuint", "GLint", "GLsizei", "GLboolean", "GLbitfield", "GLfloat", "GLclampf",
          "GLfixed", "GLubyte", "GLshort", "GLushort", "GLintptr", "GLsizeiptr"}
FLOATS = {"GLfloat", "GLclampf"}
WIDE = {"GLintptr", "GLsizeiptr"}
NAME_PARAM = {"texture": "NS_TEXTURE", "buffer": "NS_BUFFER", "framebuffer": "NS_FRAMEBUFFER",
              "renderbuffer": "NS_RENDERBUFFER", "program": "NS_PROGRAM", "shader": "NS_PROGRAM",
              "array": "NS_VERTEX_ARRAY"}
GEN = {"glGenTextures": "NS_TEXTURE", "glGenBuffers": "NS_BUFFER", "glGenFramebuffers": "NS_FRAMEBUFFER",
       "glGenRenderbuffers": "NS_RENDERBUFFER", "glGenVertexArraysOES": "NS_VERTEX_ARRAY"}
DEL = {"glDeleteTextures": ("NS_TEXTURE", "glDeleteTextures"),
       "glDeleteBuffers": ("NS_BUFFER", "glDeleteBuffers"),
       "glDeleteFramebuffers": ("NS_FRAMEBUFFER", "glDeleteFramebuffers"),
       "glDeleteRenderbuffers": ("NS_RENDERBUFFER", "glDeleteRenderbuffers"),
       "glDeleteVertexArraysOES": ("NS_VERTEX_ARRAY", "glDeleteVertexArraysOES")}
SINGLE_DEL = {"glDeleteProgram": "NS_PROGRAM", "glDeleteShader": "NS_PROGRAM"}
# Client-side state tracking (for client-memory vertex arrays / indices): the
# generated encoder calls mgl_hook_<name>(args) first.
CLIENT_HOOK = {"glBindBuffer", "glBindVertexArrayOES", "glEnableVertexAttribArray", "glDisableVertexAttribArray",
               "glDeleteBuffers", "glDeleteVertexArraysOES"}


def is_ext(n):
    return n in EXT_FUNCS


def vec_size(f):
    """Element count of the single input array of f, as a C expression, or None."""
    n, ps = f["name"], f["params"]
    m = re.match(r"glUniform([1-4])[fi]v$", n)
    if m:
        return f"count * {m.group(1)}"
    m = re.match(r"glUniformMatrix([2-4])fv$", n)
    if m:
        k = int(m.group(1))
        return f"count * {k * k}"
    m = re.match(r"glVertexAttrib([1-4])fv$", n)
    if m:
        return m.group(1)
    if n in ("glTexParameterfv", "glTexParameteriv"):
        return "1"
    if n == "glDiscardFramebufferEXT":
        return "numAttachments"
    return None


def classify(f):
    n = f["name"]
    if n in SPECIAL:
        return "special"
    if n in GEN:
        return "gen"
    if n in DEL:
        return "del"
    ps = f["params"]
    scalars = all(t in SCALAR for t, _ in ps)
    if scalars and f["ret"] == "void":
        return "async"
    if scalars and f["ret"] in ("GLboolean", "GLenum", "GLuint", "GLint"):
        return "sync_ret"
    if f["ret"] == "void" and vec_size(f) and sum(1 for t, _ in ps if t not in SCALAR) == 1:
        return "vec"
    return "special"


def enc_scalar(t, p):
    if t in FLOATS:
        return f"\tmgl_wf({p});"
    if t in WIDE:
        return f"\tmgl_w64((uint64_t)(int64_t)({p}));"
    return f"\tmgl_w32((uint32_t)({p}));"


def dec_scalar(t, p, fname):
    if t in FLOATS:
        return f"\t{t} {p} = mgl_f32(r);"
    if t in WIDE:
        return f"\t{t} {p} = ({t})(int64_t)mgl_u64(r);"
    # Object names are GLuint; a GLenum that happens to be called "texture"
    # (glActiveTexture's GL_TEXTUREi unit) must pass through unmapped.
    ns = NAME_PARAM.get(p) if t == "GLuint" else None
    if ns:
        return f"\t{t} {p} = ({t})map_name({ns}, mgl_u32(r));"
    return f"\t{t} {p} = ({t})mgl_u32(r);"


ops, cl, sv, special = [], [], [], []
for i, n in enumerate(NAMES):
    f = funcs[n]
    ops.append(f"#define OP_{n} (MGL_OP_GL_BASE + {i})")
    kind = classify(f)
    params = ", ".join(f"{t} {p}" for t, p in f["params"]) or "void"
    args = ", ".join(p for _, p in f["params"])
    call = n if not is_ext(n) else f"((PFN_{n})getproc(\"{n}\"))"
    if is_ext(n):
        sv.append(f"typedef {f['ret']} (GL_APIENTRY *PFN_{n})({params});")
    if kind == "special":
        special.append(n)
        continue
    head = f"GL_APICALL {f['ret']} GL_APIENTRY {n}({params}) {{"
    if n in CLIENT_HOOK:
        head = f"void mgl_hook_{n}({params});\n" + head + f"\n\tmgl_hook_{n}({args});"
    if kind == "async":
        cl += [head, f"\tmgl_lock();\n\tmgl_begin(OP_{n}, 0);"]
        cl += [enc_scalar(t, p) for t, p in f["params"]]
        cl += ["\tmgl_end();\n\tmgl_unlock();\n}"]
        sv += [f"static void d_{n}(struct mgl_reader *r) {{"]
        sv += [dec_scalar(t, p, n) for t, p in f["params"]]
        if n in SINGLE_DEL:
            p = f["params"][0][1]
            sv += [f"\tforget_name({SINGLE_DEL[n]}, ((uint32_t *)r->p)[-1]);"]
        sv += [f"\t{call}({args});"]
        if n == "glCompileShader":
            sv += ["\tcheck_compile(shader);"]
        if n == "glLinkProgram":
            sv += ["\tcheck_link(program);"]
        sv += ["}"]
    elif kind == "sync_ret":
        cl += [head, f"\tmgl_lock();\n\tmgl_begin(OP_{n}, 1);"]
        cl += [enc_scalar(t, p) for t, p in f["params"]]
        cl += [f"\tstruct mgl_reader rr = mgl_end_sync();\n\t{f['ret']} ret = ({f['ret']})mgl_u32(&rr);",
               "\tmgl_unlock();\n\treturn ret;\n}"]
        sv += [f"static void d_{n}(struct mgl_reader *r) {{"]
        sv += [dec_scalar(t, p, n) for t, p in f["params"]]
        sv += [f"\treply_u32((uint32_t){call}({args}));", "}"]
    elif kind == "vec":
        size = vec_size(f)
        (vt, vp), = [(t, p) for t, p in f["params"] if t not in SCALAR]
        base = vt.replace("const", "").replace("*", "").strip()
        cl += [head, f"\tmgl_lock();\n\tmgl_begin(OP_{n}, 0);"]
        for t, p in f["params"]:
            cl.append(enc_scalar(t, p) if t in SCALAR else f"\tmgl_wblob({p}, (uint32_t)(({size}) * sizeof({base})));")
        cl += ["\tmgl_end();\n\tmgl_unlock();\n}"]
        sv += [f"static void d_{n}(struct mgl_reader *r) {{"]
        for t, p in f["params"]:
            sv.append(dec_scalar(t, p, n) if t in SCALAR else f"\tconst {base} *{p} = mgl_blob(r, NULL);")
        sv += [f"\t{call}({args});", "}"]
    elif kind == "gen":
        ns = GEN[n]
        (_, cnt), (_, arr) = f["params"]
        cl += [head, "\tmgl_lock();",
               f"\tfor (GLsizei i = 0; i < {cnt}; i++) {arr}[i] = mgl_alloc_name(M{ns[0:]});",
               f"\tmgl_begin(OP_{n}, 0);\n\tmgl_wblob({arr}, (uint32_t)({cnt} * sizeof(GLuint)));",
               "\tmgl_end();\n\tmgl_unlock();\n}"]
        sv += [f"static void d_{n}(struct mgl_reader *r) {{", "\tuint32_t len;",
               "\tconst uint32_t *ids = mgl_blob(r, &len);",
               f"\tfor (uint32_t i = 0; i < len / 4; i++) create_name({ns}, ids[i]);", "}"]
    elif kind == "del":
        ns, real = DEL[n]
        (_, cnt), (_, arr) = f["params"]
        cl += [head, f"\tmgl_lock();\n\tmgl_begin(OP_{n}, 0);",
               f"\tmgl_wblob({arr}, (uint32_t)({cnt} * sizeof(GLuint)));", "\tmgl_end();\n\tmgl_unlock();\n}"]
        rcall = real if not is_ext(real) else f"((PFN_{real})getproc(\"{real}\"))"
        sv += [f"static void d_{n}(struct mgl_reader *r) {{", "\tuint32_t len;",
               "\tconst uint32_t *ids = mgl_blob(r, &len);", "\tGLuint real[256];",
               "\tfor (uint32_t i = 0; i < len / 4; i += 256) {",
               "\t\tuint32_t k = 0;",
               f"\t\tfor (; k < 256 && i + k < len / 4; k++) real[k] = map_name({ns}, ids[i + k]), forget_name({ns}, ids[i + k]);",
               f"\t\t{rcall}((GLsizei)k, real);", "\t}", "}"]

# opcode header
with open(os.path.join(OUT, "gl_ops.h"), "w") as fh:
    fh.write("/* generated by gen_remote.py */\n#ifndef MGL_GL_OPS_H\n#define MGL_GL_OPS_H\n#include \"proto.h\"\n")
    fh.write("\n".join(ops) + f"\n#define MGL_GL_NOPS {len(NAMES)}\n")
    fh.write("static const char *const mgl_gl_op_names[] = {\n" + "".join(f"\t\"{n}\",\n" for n in NAMES) + "};\n#endif\n")

# client
with open(os.path.join(OUT, "client_gl_gen.c"), "w") as fh:
    fh.write("/* generated by gen_remote.py */\n#include <GLES2/gl2.h>\n#include <GLES2/gl2ext.h>\n"
             "#include \"core.h\"\n#include \"../gl_ops.h\"\n\n")
    fh.write("\n".join(cl).replace("mgl_alloc_name(MNS_", "mgl_alloc_name(MGL_NS_"))
    fh.write("\n\n/* name -> entry point, for eglGetProcAddress */\nconst struct mgl_proc { const char *name; void *fn; } mgl_gl_procs[] = {\n")
    fh.write("".join(f"\t{{\"{n}\", (void *){n}}},\n" for n in NAMES))
    fh.write("\t{0, 0}\n};\n")

# server
with open(os.path.join(OUT, "server_gl_gen.c"), "w") as fh:
    fh.write("/* generated by gen_remote.py */\n#include \"server.h\"\n#include \"../gl_ops.h\"\n\n"
             "void *getproc(const char *name);\nvoid check_compile(GLuint shader);\nvoid check_link(GLuint program);\n"
             + "".join(f"void s_{n}(struct mgl_reader *r);\n" for n in special) + "\n")
    fh.write("\n".join(sv))
    fh.write("\n\nint dispatch_gl(uint16_t op, struct mgl_reader *r) {\n\tswitch (op) {\n")
    for n in NAMES:
        fn = f"s_{n}" if n in special else f"d_{n}"
        fh.write(f"\tcase OP_{n}: {fn}(r); return 1;\n")
    fh.write("\t}\n\treturn 0;\n}\n")

print(f"{len(NAMES)} functions; {len(special)} hand-written:", " ".join(special), file=sys.stderr)
