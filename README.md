# tizen-tv-chromium

Modern Chromium (150) on a 2017 Samsung Tizen 3.0 TV, drawing with the TV's own GPU.

The TV's built-in web engine is too old for most current web apps. This project
runs Debian's Chromium in a chroot on the TV, shows it through the TV's
compositor, wires up the remote and the speakers, and adds `mgl`, a small OpenGL
ES 2.0 remoting layer that lets Chromium render with the TV's Mali-450 GPU.

Tested on a **Samsung UN43MU6100** (Tizen 3.0, Mali-450 MP, 1.3 GB RAM). Other
2017 Tizen 3.0 sets are likely close, but untested.

## Status

Works:
- Current web apps at 1920×1080, composited and rasterized on the GPU.
- Video (decoded in software): 1080p plays at 25–30 fps, with a few dropped frames per second at most.
- Remote: arrows, OK, Back, digits, color and media keys reach the page as keyboard events.
- Sound through the TV's speakers, in sync with the picture.

Not yet:
- Typing text with the remote (only digits work).
- Starting it from the TV itself; today a PC starts it over SSH.
- Browser UI (address bar, tabs) and a launcher for web apps.

## Requirements

- **Root shell access to the TV over SSH** with a key. Getting it is out of scope here.
- A USB stick in the TV with an **ext4 partition labelled `tvchrome`** (1 GB is plenty).
  It holds the Chromium image and the browser profile.
- On the PC:
  - Docker with `linux/arm/v7` emulation (qemu-user + binfmt);
  - `mksquashfs` (squashfs-tools);
  - Go;
  - [zig](https://ziglang.org) 0.17, used as the cross compiler for the TV;
  - optionally Node 22+ and Python 3 for the tools.

## Quick start

```sh
cp tv.conf.example tv.conf      # set TV_HOST, TV_KEY, KIOSK_URL, language, time zone
./mgl/fetch-tv-libs.sh          # copies the TV's GL libraries to link against (not redistributable)
make -C mgl                     # GPU relay: client (chroot side) + server (TV side)
./kiosk/build-image.sh          # Debian armhf + Chromium + Xvfb + wlxbridge -> kiosk/build/rootfs.sqfs
./kiosk/kiosk install           # copies the image, relay and bridge to the stick
./kiosk/kiosk start [URL]       # kiosk stop | status; the remote's Exit key also stops it
```

Optionally, run `./debloat/samsung-tv-debloat` after each TV boot. It stops
telemetry, ads and other daemons the TV doesn't need, which frees RAM for Chromium.

## How it works

```
 TV userspace (Tizen 3.0, soft-float)              chroot (Debian trixie armhf, hard-float)
 ─────────────────────────────────────             ────────────────────────────────────────
 E20 Wayland compositor                            Chromium 150, --ozone-platform=x11
   ▲ GPU window      ▲ input/audio window            │ GL (ANGLE)     │ X11      │ ALSA
   │                 │                               ▼                ▼          ▼
 mgl-server ◄────────┼──── unix socket ────── libEGL.so.1 (mgl)     Xvfb   audio FIFO
   │ replays GL      │                                               ▲          │
   ▼                 │                                               │ XTEST    │
 libMali          wlxbridge ─────────── remote keys ─────────────────┘          │
                  pacat ─► PulseAudio ◄──────────────────────────────────────────┘
```

- **Display.** The TV's compositor (E20) offers only `wl_shell` and `xdg_shell` v1,
  which modern Chromium can't use. So Chromium runs on X11 against Xvfb, and the
  pieces below put its output on screen.
- **GPU, [`mgl/`](mgl/).** Chromium can't load the TV's GPU driver: the TV's
  userspace is soft-float and Debian armhf is hard-float. Instead, `libEGL.so.1`
  in the chroot serializes Chromium's EGL and GLES2 calls over a Unix socket.
  `mgl-server`, a native TV binary, replays them on the Mali and shows the frames
  as its own Wayland window. Chromium uses ANGLE's GLES backend, which accepts
  ES 2.0 once the ES3 fallback switch is off.
- **Input and audio, [`wlxbridge/`](wlxbridge/).** A native Wayland client that grabs
  the remote's keys through `tizen_keyrouter` and replays them into X with XTEST.
  It also holds the audio FIFO open. In software mode it additionally copies Xvfb's
  framebuffer into a Wayland window.
- **Sound.** Samsung's PulseAudio rejects Debian's libpulse. Chromium plays through
  ALSA, ALSA writes raw PCM into a FIFO, and the TV's own `pacat` plays it.
- **Kiosk, [`kiosk/`](kiosk/).** The image is a gzip squashfs on the stick (the TV's
  kernel has no other squashfs compressors). `kiosk-tv.sh` mounts it, sets up the
  chroot and starts everything. It is streamed over SSH because the TV refuses to
  run unsigned scripts from USB storage.

Without the relay (`KIOSK_GPU=0`) everything works in software, at roughly half
the video frame rate.

## Layout

| Path | What |
|---|---|
| `kiosk/` | image build (`build-image.sh`, Dockerfiles), PC driver (`kiosk`), TV side (`kiosk-tv.sh`) |
| `mgl/` | GLES2/EGL remoting: client, server, code generator, tests, GPU probe ([details](mgl/README.md)) |
| `wlxbridge/` | Wayland window, remote input and audio FIFO for the chroot (Go, static) |
| `debloat/` | stops a Tizen 3.0 TV's non-essential daemons for the current boot |
| `tools/` | `tvshot` (screenshot of a TV window), `tvkey` (press remote keys), `cdp.mjs` (DevTools) |

## Debugging

- Logs are on the TV in `/tmp/kiosk/log/`: `chromium.log`, `mgl.log`, `bridge.log`, `xvfb.log`, `pacat.log`.
- `tools/tvshot out.png` saves what the TV actually shows, using the compositor's
  own window dump, and prints the window stack.
- `tools/tvkey down down enter` presses remote keys, the same path as a real press.
- `MGL_CHECK=1 ./kiosk/kiosk start` logs every GL call that raises an error.
  `MGL_TRACE=/tmp/kiosk/log/trace.txt` logs every call. See [mgl/README.md](mgl/README.md).

## Platform notes

Things that cost time to find out on this TV:
- Native binaries must be built for `arm-linux-gnueabi` with glibc 2.20 and
  linked with `-z lazy`; the 2014 loader crashes on `BIND_NOW`.
- The TV's libwayland is about 1.10: use `wl_proxy_marshal(_constructor)`, not
  modern protocol headers. Clients need `XDG_RUNTIME_DIR=/run`.
- The graphics plane is alpha-blended over video, so every pixel must have alpha
  0xff or it shows through.
- The Mali-450 has no `highp` in fragment shaders and no vertex texture units.
  `mgl` papers over both.
- USB storage is mounted `noexec`, so binaries are copied to tmpfs before they run.
  Writes to the stick occasionally come back wrong, which is why `kiosk install`
  verifies checksums.

## License

MIT, see [LICENSE](LICENSE). The vendored Khronos and Wayland headers keep their own licenses ([mgl/third_party](mgl/third_party/README.md)).
