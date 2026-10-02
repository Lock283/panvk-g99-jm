# live.sh -- source this; reports harness progress to the X11 dashboard
# (cts/dashboard.py reads cts/runs/live_status.json and runs/<group>/results.tsv).
#   live_start <group> <total>
#   live_run <case-name> <command...>   runs it, maps exit 0/2/3 to Pass/Fail/NotSupported
#   live_end
LIVE_RUNS=/data/data/com.termux/files/home/cts/runs
live_status() { printf '{"group": "%s", "current": "%s", "total": %s, "running": %s, "qpa": null}\n' \
    "$LIVE_GROUP" "$1" "$LIVE_TOTAL" "$2" > "$LIVE_RUNS/live_status.json.tmp" &&
    mv "$LIVE_RUNS/live_status.json.tmp" "$LIVE_RUNS/live_status.json"; }
live_start() { LIVE_GROUP=$1; LIVE_TOTAL=$2; mkdir -p "$LIVE_RUNS/$1"; : > "$LIVE_RUNS/$1/results.tsv"; live_status "mulai" true; }
live_run() {
    local name=$1; shift; live_status "$name" true
    "$@"; local e=$? st
    case $e in 0) st=Pass;; 2) st=Fail;; 3) st=NotSupported;; 124) st=Timeout;; *) st=Crash;; esac
    printf '%s\t%s\texit %s\n' "$name" "$st" "$e" >> "$LIVE_RUNS/$LIVE_GROUP/results.tsv"
    return $e; }
live_end() { live_status "selesai" false; }
