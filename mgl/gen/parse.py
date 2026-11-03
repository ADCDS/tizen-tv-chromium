"""Parse EGL / GLES2 prototypes out of the Khronos headers.

Yields dicts: {"name", "ret", "params": [(type, name)], "api": "egl"|"gl"}.
Used by the generators for the tracing shim and the remoting client/server.
"""
import json
import re
import sys

PROTO = re.compile(
    r"^(?:GL_APICALL|EGLAPI)\s+(?P<ret>.+?)\s*(?:GL_APIENTRY|EGLAPIENTRY)\s+(?P<name>\w+)\s*\((?P<args>[^)]*)\)\s*;",
    re.M,
)


def split_param(p):
    p = " ".join(p.split())
    if p in ("void", ""):
        return None
    m = re.match(r"(.*?)(\w+)$", p)
    typ, name = m.group(1).strip(), m.group(2)
    if not typ:  # unnamed parameter, e.g. "GLenum"
        return (p, None)
    return (typ, name)


def parse(paths):
    seen, out = set(), []
    for path in paths:
        text = open(path).read()
        for m in PROTO.finditer(text):
            name = m.group("name")
            if name in seen:
                continue
            seen.add(name)
            params = [split_param(a) for a in m.group("args").split(",")]
            params = [p for p in params if p]
            for i, (t, n) in enumerate(params):
                if n is None:
                    params[i] = (t, f"a{i}")
            out.append({
                "name": name,
                "ret": " ".join(m.group("ret").split()),
                "params": params,
                "api": "egl" if name.startswith("egl") else "gl",
            })
    return out


if __name__ == "__main__":
    funcs = parse(sys.argv[1:])
    json.dump(funcs, sys.stdout, indent=1)
    print(f"\n{len(funcs)} functions", file=sys.stderr)
