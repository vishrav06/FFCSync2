#!/usr/bin/env bash
# FFCSync demonstration:
#   1. one normal request (accepted)
#   2. one invalid request (rule violation) and one malformed message
#   3. two client PROCESSES racing for the last seat of CSE2005-B1
# then graceful shutdown, invariant check and oracle comparison.
# Usage: scripts/demo.sh [build-dir]   (default: build)
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build}
SOCK=/tmp/ffcsync_demo.sock
OUT=results/demo
rm -rf "$OUT"
mkdir -p "$OUT"
LOG="$OUT/server.log"
SHOWN=0

step() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
# Prints the server log lines written since the last call, indented.
server_log() {
    sleep 0.1
    local total
    total=$(wc -l < "$LOG")
    tail -n +"$((SHOWN + 1))" "$LOG" | head -n "$((total - SHOWN))" | sed 's/^/    server| /'
    SHOWN=$total
}

step "start server: 2 workers, queue capacity 4, verbose thread/queue log"
"$BUILD/ffcsync_server" --data data --socket "$SOCK" --workers 2 --queue-capacity 4 --out "$OUT/server" --verbose > "$LOG" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT
for _ in $(seq 100); do [ -S "$SOCK" ] && break; sleep 0.05; done
server_log

step "1. normal request: S005 adds HUM1021-A2"
"$BUILD/ffcsync_client" --socket "$SOCK" --op ADD --student S005 --section HUM1021-A2 --id demo-1
server_log

step "2a. invalid request: S003 adds CSE2005-A1 without the CSE2001 prerequisite"
"$BUILD/ffcsync_client" --socket "$SOCK" --op ADD --student S003 --section CSE2005-A1 --id demo-2
server_log
step "2b. malformed message: unknown operation"
"$BUILD/ffcsync_client" --socket "$SOCK" --raw "REQ|demo-3|JOIN|S001|CSE2005-A1"
server_log

step "3. two client processes race for the LAST seat of CSE2005-B1 (capacity 1)"
"$BUILD/ffcsync_client" --socket "$SOCK" --requests data/workloads/demo_client_a.csv 2>/dev/null | sed 's/^/    client A| /' &
A=$!
"$BUILD/ffcsync_client" --socket "$SOCK" --requests data/workloads/demo_client_b.csv 2>/dev/null | sed 's/^/    client B| /' &
B=$!
wait $A $B
server_log

step "graceful shutdown (SIGTERM)"
kill -TERM $SERVER_PID
wait $SERVER_PID
trap - EXIT
server_log
[ -e "$SOCK" ] && echo "socket still present!" || echo "socket file removed: $SOCK"

step "oracle check: replay the server's admission log sequentially"
"$BUILD/ffcsync_seq" replay --data data --admission-log "$OUT/server/admission.csv" --out "$OUT/oracle" > /dev/null
diff -q "$OUT/server/results.csv" "$OUT/oracle/results.csv"
diff -q "$OUT/server/final_state.csv" "$OUT/oracle/final_state.csv"
echo "server results == sequential replay"
grep '^section,CSE2005-B1' "$OUT/server/final_state.csv" | awk -F, '{print "CSE2005-B1 seats taken: " $3 " / " $4}'
echo "outputs in $OUT/"
