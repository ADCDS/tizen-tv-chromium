# debloat-tv.sh: runs ON the TV. Stops a Tizen 3.0 Samsung TV's non-essential
# daemons for the current boot (tested on a UN43MU6100).
#
# The TV's system partition (including /etc) is read-only, so these units can't
# be masked; they come back after every real reboot (standby with Instant On
# keeps them stopped). The set has 1.3 GB of RAM and little of it free; Chromium
# needs what this frees.
#
# The TV's bash won't run script files from USB storage, so feed it on stdin:
#   bash -s -- [stop|status|start] [-n] [--wait-boot] < debloat-tv.sh
#     stop         (default) stop the units below that are running
#     status       show each unit's state plus the TV's load and available memory
#     start        undo: start them again (the same as a reboot, without rebooting)
#     -n           with stop/start: show what would change, change nothing
#     --wait-boot  first wait until the TV has finished starting up, then stop,
#                  and check again 2 and 5 minutes later for units that start late
# From a boot hook, run it in the background so it doesn't hold up the boot:
#   bash -s -- stop --wait-boot < /path/to/debloat-tv.sh > /tmp/debloat.log 2>&1 &
# From a PC, ./samsung-tv-debloat streams this file over SSH.

# Order matters: each activator (.path/.busname) comes before the service it
# activates, or it starts that service again right after the stop.
UNITS=(
  # Telemetry, ads and content recognition (ACR). Block their hosts in DNS as
  # well; stopping them frees the RAM.
  acr.path acr.service cis.service
  org.tizen.tv.clogger.busname clogger.service
  adagentd.service pushd.service psa-init.service
  media-analytics-service.service contents-analysis-service.service
  # Broadcast data service (Ginga + its Java middleware). No antenna channels
  # are tuned on this set; data-service autorun is also off in settings.
  # The busname must go first: stopping the service alone leaves D-Bus able to
  # re-activate it (systemd warns about exactly this on the stop).
  com.samsung.gingancl.gingancl.busname gingancl.service org.tizen.javamw.service
  # SmartThings device agent.
  ifw-thing.service
)

action=stop dry=0 waitboot=0
for arg in "$@"; do
  case $arg in
    stop | status | start) action=$arg ;;
    -n | --dry-run) dry=1 ;;
    --wait-boot) waitboot=1 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

avail() { awk '/^MemAvailable:/ { printf "%d MB", $2 / 1024 }' /proc/meminfo; }

# The TV has no `timeout` binary, so stops/starts are queued with --no-block and
# polled; a unit that hangs can't wedge the caller.
settled() { # $1 unit, $2 stop|start: wait for the target state. 100 s covers
  # systemd's 90 s stop timeout, after which it SIGKILLs the unit; some units
  # (contents-analysis-service) ignore SIGTERM and only go down that way.
  for _ in $(seq 100); do
    s=$(systemctl is-active "$1" 2>/dev/null)
    case $2:$s in
      stop:inactive | stop:failed | stop:unknown | start:active) echo "$s"; return 0 ;;
    esac
    sleep 1
  done
  echo "$s (still changing after 100 s)"; return 1
}

pass() { # one pass over the units; returns 1 if any unit didn't settle
  echo "$(date +%T) uptime $(cut -d. -f1 /proc/uptime)s · load $(cut -d' ' -f1-3 /proc/loadavg) · available $(avail)"
  stuck=$(ps -eo stat=,comm= | awk '$1 ~ /^D/ && $2 ~ /^(sync|mount|umount|reboot)$/' | wc -l)
  [ "$stuck" -gt 0 ] && echo "warning: $stuck sync/mount/reboot process(es) stuck in disk wait; changes may not complete (a power-cycle clears this)"
  rc=0
  for u in "${UNITS[@]}"; do
    s=$(systemctl is-active "$u" 2>/dev/null)
    case $action in
      status) printf '  %-36s %s\n' "$u" "$s"; continue ;;
      stop) [ "$s" = active ] || [ "$s" = activating ] || { printf '  %-36s %s\n' "$u" "$s"; continue; } ;;
      start) [ "$s" = active ] && { printf '  %-36s %s\n' "$u" "$s"; continue; } ;;
    esac
    if [ "$dry" = 1 ]; then printf '  %-36s %s -> would %s\n' "$u" "$s" "$action"; continue; fi
    systemctl "$action" --no-block "$u"
    r=$(settled "$u" "$action") || rc=1
    printf '  %-36s %s -> %s\n' "$u" "$s" "$r"
    sleep 1 # one unit at a time; the set has little RAM headroom
  done
  [ "$action" = status ] || [ "$dry" = 1 ] || echo "available now $(avail)"
  return $rc
}

if [ "$waitboot" = 1 ]; then
  # systemd 219 has no `is-system-running --wait`: poll (up to 15 minutes).
  echo "$(date +%T) waiting for the TV to finish starting up"
  for _ in $(seq 180); do
    case $(systemctl is-system-running 2>/dev/null) in running | degraded) break ;; esac
    sleep 5
  done
  echo "$(date +%T) startup state: $(systemctl is-system-running 2>/dev/null)"
  sleep 30
  pass
  sleep 120; pass
  sleep 180; pass
else
  echo "samsung-tv: $action$([ "$dry" = 1 ] && echo ' (dry run)')"
  pass
fi
