#!/usr/bin/env bash
# install.sh [-u]: install the tiles built by build.sh (tiles/build/*.wgt) on the
# TV with Samsung's app installer over the TV's debug bridge (sdb); -u removes
# them. Only apps installed this way can be started from the TV's home screen
# (a plain pkgcmd install runs from a shell but not from the home bar).
# Needs Developer Mode on the TV. Settings (../tv.conf): TV_HOST, the
# KIOSK_TILE_PKG_<id> package ids, TIZEN_SDB (default ~/tizen-studio/tools/sdb),
# TV_SDB (default <TV_HOST's host>:26101).
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
top=$(dirname "$here")
[ -f "$top/tv.conf" ] && . "$top/tv.conf"
SDB=${TIZEN_SDB:-$HOME/tizen-studio/tools/sdb}
host=${TV_HOST:?set TV_HOST in tv.conf}
host=${host#*@}
ip=$(getent ahostsv4 "$host" | awk 'NR == 1 { print $1 }')
DEV=${TV_SDB:-${ip:-$host}:26101}
remove=0
[ "${1:-}" = -u ] && remove=1
shopt -s nullglob
wgts=("$here"/build/*.wgt)
[ ${#wgts[@]} -gt 0 ] || { echo "nothing built: run tiles/build.sh first" >&2; exit 1; }
"$SDB" connect "$DEV" | tail -1
dir=/home/owner/share/tmp/sdk_tools
for w in "${wgts[@]}"; do
  id=$(basename "$w" .wgt)
  var=KIOSK_TILE_PKG_$id
  pkg=${!var:?no $var in tv.conf}
  if [ "$remove" = 1 ]; then
    printf '%s: ' "$id"
    "$SDB" -s "$DEV" shell 0 vd_appuninstall "$pkg" | grep -E "completed|fail" || true
    continue
  fi
  printf '%s (%s): ' "$id" "$pkg"
  "$SDB" -s "$DEV" push "$w" "$dir/$pkg.wgt" >/dev/null
  out=$("$SDB" -s "$DEV" shell 0 vd_appinstall "$pkg" "$dir/$pkg.wgt")
  if grep -q "install completed" <<< "$out"; then echo ok; else echo "failed:"; echo "$out" >&2; exit 1; fi
done
echo "Done. New tiles show up in the TV's Apps screen (pin them to the home bar from there)."
