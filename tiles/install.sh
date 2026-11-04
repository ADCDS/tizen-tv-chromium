#!/usr/bin/env bash
# install.sh [-u]: install the tiles built by build.sh (tiles/build/*.wgt) on the
# TV over SSH with the TV's package manager; -u uninstalls them.
# Settings: TV_HOST, TV_KEY and the KIOSK_TILE_PKG_<id> package ids from ../tv.conf.
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
top=$(dirname "$here")
[ -f "$top/tv.conf" ] && . "$top/tv.conf"
TV_HOST=${TV_HOST:?set TV_HOST in tv.conf}
TV_KEY=${TV_KEY:-$HOME/.ssh/id_ed25519}
tv() { ssh -i "$TV_KEY" -o BatchMode=yes -o ConnectTimeout=10 "$TV_HOST" "$@"; }
remove=0
[ "${1:-}" = -u ] && remove=1
shopt -s nullglob
wgts=("$here"/build/*.wgt)
[ ${#wgts[@]} -gt 0 ] || { echo "nothing built: run tiles/build.sh first" >&2; exit 1; }
for w in "${wgts[@]}"; do
  id=$(basename "$w" .wgt)
  var=KIOSK_TILE_PKG_$id
  pkg=${!var:?no $var in tv.conf}
  if [ "$remove" = 1 ]; then
    echo "removing $id ($pkg)"
    tv "pkgcmd -u -n $pkg -q" | tail -2
    continue
  fi
  echo "installing $id ($pkg.$id)"
  tv "cat > /tmp/$pkg.wgt" < "$w"
  # pkgcmd prints the result; "val[ok]" on success.
  if tv "pkgcmd -i -t wgt -p /tmp/$pkg.wgt -q; rm -f /tmp/$pkg.wgt" | tee /dev/stderr | grep -q 'val\[ok\]'; then
    echo "  ok"
  else
    echo "  the TV refused the package (see above). If root pkgcmd isn't allowed, install it" >&2
    echo "  through sdb instead (Tizen Studio): sdb push it, then vd_appinstall <pkgid> <path>." >&2
    exit 1
  fi
done
echo "Done. Pin the tiles to the home bar from the TV's Apps screen (Add to Home)."
