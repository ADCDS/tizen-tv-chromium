#!/usr/bin/env bash
# build-image.sh: build the kiosk's root filesystem as kiosk/build/rootfs.sqfs:
# Debian trixie armhf with Chromium, Xvfb and wlxbridge.
# Needs Docker with arm/v7 emulation (qemu-user + binfmt), Go and mksquashfs.
# The first build is slow: apt runs under emulation.
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
top=$(dirname "$here")
make -C "$top/wlxbridge"
docker build --platform linux/arm/v7 -t tvchrome:trixie "$here"
docker build --platform linux/arm/v7 -t tvchrome:final -f "$here/Dockerfile.final" "$top/wlxbridge/build"
mkdir -p "$here/build"
cid=$(docker create --platform linux/arm/v7 tvchrome:final /bin/true)
trap 'docker rm "$cid" >/dev/null' EXIT
# gzip: the TV kernel's squashfs has no xz, lz4 or zstd support.
docker export "$cid" | mksquashfs - "$here/build/rootfs.sqfs" -tar -comp gzip -noappend -quiet -no-progress
ls -la "$here/build/rootfs.sqfs"
