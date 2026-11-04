# kiosk-tv.sh: runs ON the TV, streamed over SSH by ./kiosk (the TV's bash
# won't run unsigned script files from USB storage, so this never lives there).
# Chromium draws into Xvfb; wlxbridge shows that screen as a wl_shell window on
# the TV's compositor and feeds the remote's keys into X. The image and the
# Chromium profile live on the stick's ext4 partition (LABEL=tvchrome) in kiosk/.
#   start [URL] | stop | status | storedir   (the Exit key on the remote also stops it)
# Settings come from the environment (./kiosk passes them from tv.conf):
#   KIOSK_URL    page opened by `start` without a URL
#   KIOSK_GPU    1 (default): render on the TV's GPU through mgl when installed; 0: software
#   KIOSK_W/H    screen size in pixels (default 1920x1080)
#   KIOSK_SCALE  device scale factor (default 2: pages lay out at 960x540 CSS pixels)
#   KIOSK_LANG   Chromium UI and Accept-Language, e.g. pt_BR (default en_US)
#   KIOSK_TZ     time zone, e.g. America/Sao_Paulo (default UTC)
#   KIOSK_SPATNAV 1 (default): arrow keys move between links on pages without their own
#                key handling (Chromium's spatial navigation); 0: off
#   KIOSK_LEAVE  host:port of the TV helper (tvkioskd): the remote's Exit key asks it
#                to hide the kiosk instead of stopping it
set -u
R=/tmp/kiosk/root
L=/tmp/kiosk/log # outside the chroot so logs survive stop
W=${KIOSK_W:-1920} H=${KIOSK_H:-1080} SCALE=${KIOSK_SCALE:-2}
LANG_UI=${KIOSK_LANG:-en_US} TZ_UI=${KIOSK_TZ:-UTC}
# Remote keys the kiosk takes from the TV (TV keymap codes): Back, digits, OK,
# color keys, channel up/down, arrows, media keys, Exit. Grabbed in "topmost"
# mode by whichever process owns the window on screen, so they go back to the
# TV whenever its own UI (Source, Home, other apps) is on top.
KEYS=9,10,11,12,13,14,15,16,17,18,19,36,67,68,69,70,95,96,111,113,114,116,174,176,182,208,209,216,252
UA="Mozilla/5.0 (SMART-TV; Linux; Tizen 3.0) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36"

mounted() { grep -q " $1 " /proc/mounts; }

store() { # mountpoint of the tvchrome partition (mount it if the TV hasn't)
  dev=$(blkid -L tvchrome) || { echo "no partition labelled tvchrome" >&2; exit 1; }
  mp=$(awk -v d="$dev" '$1 == d { print $2; exit }' /proc/mounts)
  if [ -z "$mp" ]; then
    mp=/tmp/kiosk/store
    mkdir -p $mp && mount -t ext4 -o noatime "$dev" $mp || exit 1
  fi
  echo "$mp/kiosk"
}

inroot() { # pids of processes running inside the chroot
  for p in /proc/[0-9]*; do
    [ "$(readlink "$p/root" 2>/dev/null)" = "$R" ] && echo "${p#/proc/}"
  done
}

# On Linux Chromium takes its UI language from LANGUAGE (--lang is ignored).
ch() { chroot "$R" /usr/bin/env -i PATH=/usr/bin:/bin HOME=/data LANG=C.UTF-8 LANGUAGE="$LANG_UI" TZ="$TZ_UI" "$@"; }

up() {
  dir=$(store) || exit 1
  mkdir -p "$R" "$dir/data"
  mounted "$R" || mount -t squashfs -o loop,ro "$dir/rootfs.sqfs" "$R" || exit 1
  mounted "$R/proc" || mount -t proc proc "$R/proc"
  mounted "$R/sys" || mount -t sysfs sysfs "$R/sys"
  mounted "$R/dev" || mount --bind /dev "$R/dev"
  mounted "$R/dev/shm" || mount --bind /dev/shm "$R/dev/shm"
  mounted "$R/tmp" || mount -t tmpfs -o size=128m,mode=1777 tmpfs "$R/tmp"
  mounted "$R/run" || mount -t tmpfs -o size=4m,mode=755 tmpfs "$R/run"
  mkdir -p "$R/run/pulse"
  mounted "$R/run/pulse" || mount --bind /run/pulse "$R/run/pulse"
  [ -e "$R/run/wayland-0" ] || : > "$R/run/wayland-0"
  mounted "$R/run/wayland-0" || mount --bind /run/wayland-0 "$R/run/wayland-0"
  mounted "$R/data" || mount --bind "$dir/data" "$R/data"
  mounted "$R/etc/resolv.conf" || mount --bind /opt/etc/resolv.conf "$R/etc/resolv.conf"
  mounted "$R/etc/hosts" || mount --bind /etc/hosts "$R/etc/hosts"
  # Extensions (tvkit) live on the stick, so updating them never rewrites the image.
  mkdir -p "$dir/opt"
  mounted "$R/opt" || mount --bind "$dir/opt" "$R/opt"
  # Managed policies: no bubbles that would steal the remote's focus. The image
  # is read-only and has no policies/ dir, so /etc/chromium comes from the stick.
  if ! mounted "$R/etc/chromium"; then
    mkdir -p "$dir/etc-chromium/policies/managed"
    cp "$R/etc/chromium/master_preferences" "$dir/etc-chromium/" 2>/dev/null
    cat > "$dir/etc-chromium/policies/managed/kiosk.json" <<'JSON'
{
  "TranslateEnabled": false,
  "PasswordManagerEnabled": false,
  "AutofillAddressEnabled": false,
  "AutofillCreditCardEnabled": false,
  "BrowserSignin": 0,
  "SyncDisabled": true,
  "MetricsReportingEnabled": false,
  "SearchSuggestEnabled": false,
  "DefaultNotificationsSetting": 2,
  "DefaultGeolocationSetting": 2
}
JSON
    mount --bind "$dir/etc-chromium" "$R/etc/chromium"
  fi
}

stop() {
  for pf in pacat mgl; do
    if [ -f "$L/$pf.pid" ]; then kill "$(cat "$L/$pf.pid")" 2>/dev/null; rm -f "$L/$pf.pid"; fi
  done
  pids=$(inroot)
  if [ -n "$pids" ]; then
    kill $pids 2>/dev/null
    for _ in $(seq 25); do [ -z "$(inroot)" ] && break; sleep 0.2; done
    pids=$(inroot); [ -n "$pids" ] && kill -9 $pids 2>/dev/null
  fi
  for m in opt etc/chromium etc/hosts etc/resolv.conf data run/wayland-0 run/pulse run tmp dev/shm dev sys proc ""; do
    mounted "$R${m:+/$m}" && { umount "$R${m:+/$m}" 2>/dev/null || umount -l "$R${m:+/$m}"; }
  done
  return 0
}

run() { # foreground: start everything, wait for the bridge, then tear down
  url=$1
  up
  mkdir -p "$L"; rm -f "$L"/*.log
  echo 1000 > /proc/self/oom_score_adj # inherited: OOM kills us before the TV's processes
  ch Xvfb :1 -screen 0 "${W}x${H}x24" -fbdir /tmp -nolisten tcp -ac -noreset </dev/null >"$L/xvfb.log" 2>&1 &
  for _ in $(seq 50); do [ -S "$R/tmp/.X11-unix/X1" ] && [ -f "$R/tmp/Xvfb_screen0" ] && break; sleep 0.2; done
  [ -S "$R/tmp/.X11-unix/X1" ] || echo "Xvfb did not come up in 10 s"
  # A newer bridge next to the image overrides the one baked into it; the stick
  # is mounted noexec, so it runs from the chroot's tmpfs.
  # GPU mode needs the mgl relay (./kiosk install puts it next to the image).
  gpu=0
  [ "${KIOSK_GPU:-1}" = 1 ] && [ -f "$dir/mgl-server" ] && [ -f "$dir/libmgl.so" ] && gpu=1
  bridge_bin=/usr/local/bin/wlxbridge
  if [ -f "$dir/wlxbridge" ]; then
    cp "$dir/wlxbridge" "$R/tmp/wlxbridge" && chmod 755 "$R/tmp/wlxbridge" && bridge_bin=/tmp/wlxbridge
  fi
  if [ "${KIOSK_NOBRIDGE:-0}" = 1 ]; then # test only: no input/audio bridge window
    sleep 100000 & bridge=$!
  else
    # GPU mode: mgl-server's window is the one on screen, so it grabs the keys
    # and passes them to the bridge over its window socket; the bridge shows no
    # window. Software mode: the bridge's own window shows Xvfb and grabs them.
    bridge_keys="-grab $KEYS"
    [ "$gpu" = 1 ] && bridge_keys="-keys-socket /tmp/mglwin.sock"
    [ -n "${KIOSK_LEAVE:-}" ] && bridge_keys="$bridge_keys -leave $KIOSK_LEAVE"
    ch "$bridge_bin" -display :1 -fb /tmp/Xvfb_screen0 -wayland /run/wayland-0 -audio-fifo /tmp/audio.fifo -v $bridge_keys \
      </dev/null >"$L/bridge.log" 2>&1 &
    bridge=$!
  fi
  # Audio: the TV's Samsung-patched PulseAudio drops Debian's libpulse at stream
  # creation, and the TV's own (soft-float) libpulse can't load into Debian's
  # hard-float Chromium. So Chromium plays to ALSA, ALSA writes raw PCM into a
  # FIFO (held open by the bridge), and the TV's native pacat plays the FIFO.
  cat > "$dir/data/.asoundrc" <<'ALSA'
# Written by kiosk-tv.sh: default ALSA device = raw PCM into the audio FIFO.
pcm.!default {
	type plug
	slave { pcm "kioskfifo"; format S16_LE; rate 48000; channels 2 }
}
pcm.kioskfifo {
	type file
	slave.pcm "null"
	file "/tmp/audio.fifo"
	format "raw"
}
ALSA
  for _ in $(seq 25); do [ -p "$R/tmp/audio.fifo" ] && break; sleep 0.2; done
  PULSE_SERVER=unix:/run/pulse/native pacat --playback --raw --format=s16le --rate=48000 --channels=2 \
    --latency-msec=60 --stream-name=Chromium --client-name=tv-kiosk "$R/tmp/audio.fifo" </dev/null >"$L/pacat.log" 2>&1 &
  echo $! > "$L/pacat.pid"
  # Rendering: in GPU mode Chromium's GL goes through the mgl relay (otherwise
  # software): libEGL.so.1 in the image is a symlink to /tmp/mgl/libEGL.so.1
  # (the relay client); mgl-server (TV-native) replays the calls on the Mali and
  # shows Chromium's window as its own Wayland surface. ANGLE needs the ES3
  # fallback switch off to accept the Mali's ES 2.0.
  gl_flags="--disable-gpu --disable-software-rasterizer"
  feat="Translate,HardwareMediaKeyHandling,MediaRouter,DialMediaRouteProvider,GlobalMediaControls,OptimizationHints"
  if [ "$gpu" = 1 ]; then
    mkdir -p "$R/tmp/mgl"
    cp "$dir/libmgl.so" "$R/tmp/mgl/libEGL.so.1"
    cp "$dir/mgl-server" "$L/mgl-server" && chmod 755 "$L/mgl-server"
    MGL_RING="$L/mgl.ring" MGL_WINDOW_SOCKET="$R/tmp/mglwin.sock" MGL_GRAB_KEYS="$KEYS" XDG_RUNTIME_DIR=/run \
      "$L/mgl-server" "$R/tmp/mgl.sock" -v </dev/null >"$L/mgl.log" 2>&1 &
    echo $! > "$L/mgl.pid"
    for _ in $(seq 25); do [ -S "$R/tmp/mgl.sock" ] && break; sleep 0.2; done
    gl_flags="--use-gl=angle --use-angle=gles-egl --ignore-gpu-blocklist --disable-gpu-watchdog"
    feat="$feat,FallbackToSWIfGLES3NotSupported"
  fi
  # tvkit (on-screen keyboard, browser bar, Back handling, cursor) from the stick.
  ext_flags=""
  [ -f "$R/opt/tvkit/manifest.json" ] && ext_flags="--load-extension=/opt/tvkit"
  [ "${KIOSK_SPATNAV:-1}" = 1 ] && ext_flags="$ext_flags --enable-spatial-navigation"
  # --window-size is in DIPs (scaled by the device scale factor), so divide,
  # or the window overflows the screen and the right/bottom of the page is cut.
  wd=$(awk -v v="$W" -v s="$SCALE" 'BEGIN { printf "%d", v / s + 0.5 }')
  hd=$(awk -v v="$H" -v s="$SCALE" 'BEGIN { printf "%d", v / s + 0.5 }')
  ch DISPLAY=:1 PULSE_SERVER=unix:/nonexistent chromium \
    --ozone-platform=x11 --no-sandbox --kiosk --window-position=0,0 --window-size="$wd,$hd" \
    --force-device-scale-factor="$SCALE" --user-agent="$UA" \
    --user-data-dir=/data/profile --disk-cache-dir=/data/cache --disk-cache-size=67108864 \
    $gl_flags --autoplay-policy=no-user-gesture-required \
    --no-first-run --no-default-browser-check --noerrdialogs --disable-infobars \
    --password-store=basic --disable-background-networking --disable-component-update --disable-breakpad \
    --disable-features="$feat" \
    --renderer-process-limit=2 --hide-scrollbars $ext_flags \
    --remote-debugging-address=127.0.0.1 --remote-debugging-port=9222 "$url" </dev/null >"$L/chromium.log" 2>&1 &
  while kill -0 "$bridge" 2>/dev/null; do sleep 2; done # Exit key -> bridge exits
  echo "bridge exited; stopping"
  stop
}

case ${1:-} in
  start)
    if [ -n "$(inroot)" ]; then echo "already running"; exit 0; fi
    url=${2:-${KIOSK_URL:-about:blank}}
    # Forked subshell (keeps these functions; no new script file to exec),
    # detached from the SSH session: HUP ignored, all fds off the channel.
    ( trap '' HUP; exec </dev/null >/tmp/kiosk.log 2>&1; run "$url" ) &
    echo "started: $url (log: /tmp/kiosk.log)" ;;
  stop) stop; echo stopped ;;
  storedir) d=$(store) && mkdir -p "$d" && echo "$d" ;; # where ./kiosk install puts files
  status)
    pids=$(inroot)
    echo "processes in chroot: $(echo $pids | wc -w)"
    [ -n "$pids" ] && ps -o pid,rss,args -p "$(echo $pids | tr ' ' ',')" | cut -c1-120
    awk '/^MemAvailable/ {printf "available %d MB\n", $2/1024}' /proc/meminfo
    [ -f "$L/pacat.pid" ] && kill -0 "$(cat "$L/pacat.pid")" 2>/dev/null && echo "audio relay (native pacat): running" || echo "audio relay (native pacat): NOT running"
    for f in bridge xvfb chromium pacat mgl; do
      [ -f "$L/$f.log" ] && { echo "--- $f.log"; tail -n 8 "$L/$f.log"; }
    done
    [ -f /tmp/kiosk.log ] && { echo "--- kiosk.log"; tail -n 4 /tmp/kiosk.log; } ;;
  *) echo "usage: kiosk start [URL] | stop | status | storedir"; exit 2 ;;
esac
