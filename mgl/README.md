# mgl: Chromium's GPU code on the TV's Mali-450

Chromium in the chroot is Debian armhf (hard-float). The TV's GPU driver (libMali)
is soft-float and tied to Tizen's window system, so Chromium can't load it. `mgl`
splits the GL stack across that boundary:

- **`libEGL.so.1`** (client, armhf) sits in the chroot where ANGLE expects the
  system EGL. It exports EGL 1.4 and the OpenGL ES 2.0 functions, encodes each
  call into a buffer and sends it over a Unix socket.
- **`mgl-server`** (TV-native, soft-float) decodes the calls and replays them on
  libMali. An EGL window surface becomes a `wl_shell` window on the TV's compositor
  through wayland-egl, so frames go from the GPU to the screen without a CPU copy.

Chromium runs with `--use-gl=angle --use-angle=gles-egl --ignore-gpu-blocklist
--disable-gpu-watchdog --disable-features=FallbackToSWIfGLES3NotSupported`.
`../kiosk/kiosk-tv.sh` sets this up.

## Build

```sh
./fetch-tv-libs.sh   # once: the TV's libEGL/libGLESv2/libwayland-* into sysroot/ (not redistributable)
make                 # build/armhf/libEGL.so.1 (client) and build/tv/mgl-server
make pc              # PC build (server on Mesa) + test/relaytest, for development
make glprobe         # build/tv/glprobe: native GPU probe and benchmark
make gen             # regenerate src/gl_ops.h and src/*/gl_gen.c from the GLES2 headers
```

`gen/gen_remote.py` writes the encoder and decoder for each GLES2 function. The
ones that need more than copying arguments are hand-written in
`src/client/gl_special.c` and `src/server/gl_special.c`: pixel and buffer data,
queries, shader sources, and draws with client-side arrays.

## Protocol

- Each message is `[u16 op][u16 flags][u32 size][payload]`. Messages are batched
  and flushed on calls that return a value, on swaps, `glFlush`/`glFinish`, or a
  full buffer. Calls that return data block for the reply.
- Object names (textures, buffers, framebuffers, renderbuffers, programs, shaders,
  VAOs) are allocated by the client and mapped by the server, so `glGen*` never
  waits for the server. Framebuffers and VAOs are per context; the rest are per
  share group.
- One connection, one server thread. The client sends a MakeCurrent whenever the
  calling thread's (context, draw, read) differs from the last one sent.
- When Chromium restarts its GPU process, the new connection replaces the old one.

## What it fixes up for Chromium

- **Client-side arrays.** ES 2.0 lets vertex attributes and indices come from
  client memory, and ANGLE uses that. The client tracks where each attribute
  comes from and sends the data with each draw. It keeps a copy of every buffer's
  contents to work out an indexed draw's vertex range, because Skia fills its
  shared quad index buffer through `GL_ARRAY_BUFFER` before binding it as the
  element buffer.
- **No fragment `highp`.** The client reports a highp-capable fragment
  precision, which ANGLE requires. The server rewrites `highp` to `mediump` in
  every shader, so that uniforms shared by both stages keep matching precisions.
- **No vertex texture units.** The Mali reports 0, which fails ANGLE's link
  check. The server reports 4.
- **EGL configs.** Only configs usable for ES 2.0 with a pbuffer are offered.
  `EGL_NATIVE_VISUAL_ID` reports the X root visual, which Chromium matches
  against its X11 window. `EGL_COLOR_COMPONENT_TYPE_EXT` is answered locally.
- **Fences.** All GL runs in order on one server thread, and the client copies
  data at call time. So a fence only needs a `glFlush`; `MGL_FENCE_FINISH=1`
  makes it a `glFinish`.
- **Alpha.** The TV blends graphics over video by alpha. Before each swap the
  server forces the window's alpha to 1 (`MGL_KEEP_ALPHA=1` turns that off).

## Window control (kiosk GPU mode)

In GPU mode the relay's window is the one on screen, so it also owns the remote.
This is enabled with `MGL_WINDOW_SOCKET`:
- **Keys.** It grabs `MGL_GRAB_KEYS` (TV keymap codes) through `tizen_keyrouter`
  in *topmost* mode. Keys reach us only while our window is on top. When the TV's
  own UI (Source, Home, another app) covers it, the keys go back to the TV.
- **Socket output.** Clients of the socket get these lines:
  - `key <evdev> <0|1>`
  - `focus <0|1>`
  - `visible <0|1>`, from `tizen_visibility`
- **Socket input.** Clients may send `show` or `hide` (`tizen_policy`
  uniconify+activate / iconify).
- **Hidden window.** Swaps are skipped while the window is hidden or fully
  covered. The compositor sends no frame callbacks then, so a swap would block
  the whole server.

The Tizen protocols are loaded at run time from the TV's
`libtizen-extension-client.so.0`, and requests are looked up by name.
`wlxbridge -keys-socket` replays the keys into X.

## Settings and debugging (environment)

| Variable | Side | Effect |
|---|---|---|
| `MGL_SOCKET` | client | socket path (default `/tmp/mgl.sock`) |
| `MGL_WINDOW_SIZE` | client | window size when the X window's size can't be read (`WxH`) |
| `MGL_CHECK=1` | server | log every GL call that raises an error, with its first arguments |
| `MGL_TRACE=file` | server | log every call with its first arguments |
| `MGL_RING=file` | server | keep the last 64 calls in an mmapped file (survives a crash) |
| `MGL_OUTPUT_SIZE` | server | scale the window to `WxH` on screen (`wl_scaler`) |
| `MGL_FENCE_FINISH=1` | server | fences wait for the GPU (`glFinish`) |
| `MGL_KEEP_ALPHA=1` | server | don't force opaque alpha before swaps |
| `MGL_WINDOW_SOCKET` | server (TV) | window control socket: keys, focus, visibility, show/hide |
| `MGL_GRAB_KEYS` | server (TV) | comma-separated TV keymap codes to grab while our window is on top |
| `MGL_DUMP_DIR`, `MGL_DUMP_EVERY` | PC server | write every Nth frame as PPM |

`../kiosk/kiosk` passes `MGL_CHECK`, `MGL_TRACE`, `MGL_OUTPUT_SIZE` and
`MGL_FENCE_FINISH` through to the TV. `mgl-server -v` (always on in the kiosk)
logs configs, contexts, surfaces and the frame rate.

## Performance

- 1080p video plays at 25–30 fps, against 12–15 fps with software compositing.
- The compositor's CPU use drops from about 65% to about 15%.
- Costs that remain:
  - every texture upload is copied through the socket;
  - `glGetError` and other queries are round trips.

  Shared memory for large uploads and fewer queries are the obvious next steps.
