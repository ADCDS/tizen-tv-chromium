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
  When the TV's own UI (Source, Home, another app) is on top, the remote goes back to the TV.
- Sound through the TV's speakers, in sync with the picture.
- Apps as TV tiles: each configured web app gets its own tile in the TV's app
  list. Opening a tile shows that app in its own Chromium tab, starting the
  kiosk on demand (about 10 s cold, instant afterwards).
- Leaving with Back or Exit hides Chromium and pauses media, keeping it loaded.
- Opening the TV's own Internet app opens this browser instead, on a start page
  with a search/address box.
- On-screen keyboard, a browser bar, and a pointer mode for sites that need a mouse.

## The remote

| Button | In the kiosk |
|---|---|
| Arrows, OK | move and select; on pages without their own TV navigation, arrows jump between links |
| OK on a text field | opens the on-screen keyboard (arrows + OK to type, Back to close, digits type directly) |
| Back | the page's own Back; with no history left, leave (hide the kiosk, back to the TV) |
| Red **A** | browser bar: Back, Forward, Reload, Home, Address, Exit |
| Green **B** | pointer mode: arrows move the mouse, OK clicks, channel up/down scroll, Back ends it |
| Exit | leave |

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
./kiosk/kiosk install           # copies the image, relay, bridge, helper, extension and settings to the stick
./kiosk/kiosk daemon start      # runs the TV helper (tvkioskd) until the next reboot
./kiosk/kiosk start [URL]       # or start it directly: kiosk stop | status
```

The helper (`tvkioskd`) is what the tiles and the Internet takeover talk to. It
has to run after every TV boot. Add this to whatever already runs at boot on
your set, with DIR being the stick's `kiosk/` directory:

```sh
cp DIR/tvkioskd /tmp/ && /tmp/tvkioskd -dir DIR > /tmp/tvkioskd.log 2>&1 &
```

### Tiles

The tiles are small TV web apps, one per `KIOSK_APPS` entry, that ask the
helper to show their app. Building them needs the Tizen Studio CLI and a
certificate profile your TV accepts (`TIZEN_PROFILE` in `tv.conf`):

```sh
./tiles/build.sh                # tiles/build/<id>.wgt, signed
./tiles/install.sh              # installs them on the TV (-u removes them)
```

They must be installed through Samsung's app installer (`sdb` + `vd_appinstall`,
which `tiles/install.sh` falls back to describing): packages installed with plain
`pkgcmd` can be started from a shell but not from the TV's home screen. Then pin
them to the home bar from the TV's Apps screen.

### Memory

The set has 1.3 GB, and Samsung's own UI takes most of it as the day goes on.
Chromium needs roughly 200-300 MB. When the TV runs short, its app launcher
(which also starts the tiles) hangs or fails, and Chromium starts very slowly or
is killed. So the helper stops a hidden kiosk when less than 150 MB is
available (`-min-free`), Chromium runs in low-end device mode (`KIOSK_LOWEND`),
and the debloat matters.

Optionally, debloat after each TV boot. This stops telemetry, ads and other
daemons the TV doesn't need, which frees RAM for Chromium. Either:
- run `./debloat/samsung-tv-debloat` from the PC; or
- have the TV do it at boot. Copy `debloat/debloat-tv.sh` to the TV with
  `./debloat/samsung-tv-debloat install DIR`, then add this to whatever already
  runs at boot on your set:

  ```sh
  bash -s -- stop --wait-boot < DIR/debloat-tv.sh > /tmp/debloat.log 2>&1 &
  ```

  It waits for startup to finish, then checks twice more for daemons that start late.

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
| `debloat/` | stops a Tizen 3.0 TV's non-essential daemons for the current boot (from a PC or a boot hook) |
| `tvkioskd/` | helper on the TV: opens apps (one tab each), hides/shows the kiosk, starts it on demand, Internet takeover, start page |
| `kiosk/tvkit/` | Chromium extension: on-screen keyboard, browser bar, Back handling, pointer cursor |
| `tiles/` | TV app tiles that open the configured apps (template, build, install) |
| `tvremote/`, `tools/tvremote` | see and drive the TV from a PC: screen pictures + remote control in a browser page |
| `tools/` | `tvshot` (screenshot of a TV window), `tvkey` (press remote keys), `cdp.mjs` (DevTools) |

## Debugging

- Logs are on the TV in `/tmp/kiosk/log/`: `chromium.log`, `mgl.log`, `bridge.log`, `xvfb.log`, `pacat.log`.
  The helper logs to `/tmp/tvkioskd.log`.
- `tools/tvshot out.png` saves what the TV actually shows, using the compositor's
  own window dump, and prints the window stack.
- `tools/tvkey down down enter` presses remote keys, the same path as a real press.
- `tools/tvremote` (after `make -C tvremote`) serves http://localhost:8790: the TV's
  screen and a remote control (buttons and keyboard). The picture is rebuilt from
  the compositor's window dump and updates after each key press, on Refresh, or
  every 10 s if ticked. It doesn't stream: each picture pauses the TV's menus for
  about 1.5 s. `tvremote shot FILE` and `tvremote key NAME...` do the same from the
  command line.
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
- Don't use the compositor's capture interfaces (`tizen_screenmirror`, `screenshooter`)
  for repeated captures. Into a shared-memory buffer, each frame leaks one or two
  full-screen graphics buffers (8 MB each), even after the client disconnects, and
  the TV reboots once memory runs out (about 40 frames). Into a DRM dumb buffer
  shared through `wl_tbm`, the first capture reboots the TV. The window dump
  (`enlightenment_info -dump_topvwins`) frees everything, but it blocks the
  compositor while it runs.
- The TV mounts USB ext4 without write barriers. A power cut while Chromium
  writes its profile can corrupt the filesystem, and with a corrupt block bitmap
  new writes can land on the image. If `kiosk install` keeps re-copying an
  unchanged image, run `fsck.ext4` on the partition (kiosk stopped, unmounted).

## License

MIT, see [LICENSE](LICENSE). The vendored Khronos and Wayland headers keep their own licenses ([mgl/third_party](mgl/third_party/README.md)).
