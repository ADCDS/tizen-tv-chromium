#!/usr/bin/env bash
# fetch-tv-libs.sh: copy the TV's EGL, GLES and Wayland client libraries into
# mgl/sysroot/ so mgl-server and glprobe can link against them. They are the
# TV's own (proprietary) libraries, so they are not part of this repository.
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
top=$(dirname "$here")
[ -f "$top/tv.conf" ] && . "$top/tv.conf"
TV_HOST=${TV_HOST:?set TV_HOST in tv.conf (e.g. root@192.168.1.50)}
TV_KEY=${TV_KEY:-$HOME/.ssh/id_ed25519}
mkdir -p "$here/sysroot"
for lib in libEGL.so.1 libGLESv2.so.2 libwayland-client.so.0 libwayland-egl.so.1; do
  ssh -i "$TV_KEY" -o BatchMode=yes -o ConnectTimeout=10 "$TV_HOST" "cat /usr/lib/$lib" > "$here/sysroot/$lib"
  ln -sf "$lib" "$here/sysroot/${lib%.so.*}.so"
  echo "sysroot/$lib $(stat -c %s "$here/sysroot/$lib") bytes"
done
