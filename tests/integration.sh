#!/usr/bin/env bash
# Two-process integration test with the real executables:
#   server (4 workers) + three concurrent client processes
#   -> stop with SIGTERM -> socket removed -> replay the admission log with
#   the sequential oracle -> results and final state must be identical.
# Usage: tests/integration.sh <build-dir> <source-dir>
set -euo pipefail
BUILD=$(cd "$1" && pwd)
SRC=$(cd "$2" && pwd)
TMP=$(mktemp -d)
SOCK="$TMP/ffcsync.sock"
trap 'kill $SERVER_PID 2>/dev/null || true; rm -rf "$TMP"' EXIT

cd "$SRC"
python3 -I scripts/gen_workload.py --n 600 --seed 11 --prefix A --out "$TMP/a.csv"
python3 -I scripts/gen_workload.py --n 600 --seed 12 --prefix B --out "$TMP/b.csv"
python3 -I scripts/gen_workload.py --n 600 --seed 13 --prefix C --out "$TMP/c.csv"

"$BUILD/ffcsync_server" --data data --socket "$SOCK" --workers 4 --queue-capacity 8 \
    --out "$TMP/server" > "$TMP/server.log" &
SERVER_PID=$!
for _ in $(seq 100); do [ -S "$SOCK" ] && break; sleep 0.05; done
[ -S "$SOCK" ] || { echo "server did not start"; exit 1; }

for c in a b c; do
    "$BUILD/ffcsync_client" --socket "$SOCK" --requests "$TMP/$c.csv" --quiet --out "$TMP/$c.out" &
done
wait %2 %3 %4

kill -TERM $SERVER_PID
wait $SERVER_PID
[ ! -e "$SOCK" ] || { echo "socket file not removed"; exit 1; }
grep -q "invariants: OK" "$TMP/server/summary.txt"

"$BUILD/ffcsync_seq" replay --data data --admission-log "$TMP/server/admission.csv" \
    --out "$TMP/oracle" > /dev/null
diff "$TMP/server/results.csv" "$TMP/oracle/results.csv"
diff "$TMP/server/final_state.csv" "$TMP/oracle/final_state.csv"

total=$(($(wc -l < "$TMP/server/results.csv") - 1))
[ "$total" -eq 1800 ] || { echo "expected 1800 results, got $total"; exit 1; }
echo "integration OK: 1800 requests from 3 client processes match the sequential replay"
