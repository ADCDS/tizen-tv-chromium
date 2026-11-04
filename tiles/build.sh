#!/usr/bin/env bash
# build.sh: one TV app ("tile") per KIOSK_APPS entry in ../tv.conf, as a signed
# .wgt in tiles/build/. Opening a tile shows that app in the kiosk (through the
# TV helper, tvkioskd), so the apps sit in the TV's own app list and home bar.
# Settings (tv.conf):
#   TIZEN_PROFILE  Tizen security profile to sign with: a certificate your TV accepts
#   TIZEN_CLI      the tizen CLI (default ~/tizen-studio/tools/ide/bin/tizen)
#   KIOSK_TILE_PKG_<id>  package id per app (10 letters/digits); generated and
#                  appended to tv.conf when missing, so rebuilding updates the same app
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
top=$(dirname "$here")
conf=$top/tv.conf
[ -f "$conf" ] && . "$conf"
TIZEN=${TIZEN_CLI:-$HOME/tizen-studio/tools/ide/bin/tizen}
PROFILE=${TIZEN_PROFILE:?set TIZEN_PROFILE in tv.conf (see: tizen security-profiles list)}
[ -x "$TIZEN" ] || { echo "no tizen CLI at $TIZEN (set TIZEN_CLI)" >&2; exit 1; }
mkdir -p "$here/build"
IFS=';' read -ra entries <<< "${KIOSK_APPS:-}"
for e in "${entries[@]}"; do
  IFS='|' read -r id name url icon keep <<< "$e"
  id=${id// /} name=${name## } icon=${icon// /}
  [[ $id =~ ^[a-z0-9]+$ ]] || { echo "app id '$id': use lowercase letters and digits only" >&2; exit 1; }
  var=KIOSK_TILE_PKG_$id
  pkg=${!var:-}
  if [ -z "$pkg" ]; then
    pkg=$(tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 10)
    printf '%s=%s\n' "$var" "$pkg" >> "$conf"
    echo "new package id for $id: $pkg (saved in tv.conf)"
  fi
  d=$here/build/$id
  rm -rf "$d" && mkdir -p "$d"
  esc() { printf '%s' "$1" | sed -e 's/[&|\\]/\\&/g'; }
  for f in config.xml index.html; do
    sed -e "s|@ID@|$id|g" -e "s|@PKG@|$pkg|g" -e "s|@NAME@|$(esc "$name")|g" "$here/template/$f" > "$d/$f"
  done
  python3 -I "$here/make_icons.py" "${icon/#\~/$HOME}" "$name" "$d"
  "$TIZEN" package -t wgt -s "$PROFILE" -- "$d" | tail -2
  mv "$d"/*.wgt "$here/build/$id.wgt"
  echo "built tiles/build/$id.wgt ($pkg.$id)"
done
