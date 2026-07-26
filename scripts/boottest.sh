#!/usr/bin/env bash
# Boot X OS headless in QEMU for a fixed time, capture the serial log, and
# check it for failures.  Used by `make boottest` as a regression gate for
# kernel changes.
#
# usage: boottest.sh <qemu-binary> <seconds> [qemu args...]
set -uo pipefail

QEMU="$1"; shift
SECS="$1"; shift

LOG="$(mktemp -t xos-boottest)"
trap 'rm -f "$LOG"' EXIT

echo ">> boottest: booting headless for ${SECS}s"

# -serial file: keeps stdout clean and avoids QEMU wanting a tty.
"$QEMU" "$@" -serial "file:$LOG" >/dev/null 2>&1 &
QPID=$!

for _ in $(seq 1 "$SECS"); do
    sleep 1
    kill -0 "$QPID" 2>/dev/null || break
done
kill -9 "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "---------------- serial log ----------------"
cat "$LOG"
echo "--------------------------------------------"

fail=0
note() { echo "   $1"; }

# --- hard failures --------------------------------------------------------
if grep -qE 'CPU EXCEPTION|PANIC|panic:|Assertion failed' "$LOG"; then
    echo ">> FAIL: kernel exception or panic"
    grep -nE 'CPU EXCEPTION|PANIC|panic:|Assertion failed' "$LOG" | head -20
    fail=1
fi

if grep -q 'halting' "$LOG"; then
    echo ">> FAIL: a CPU halted on a fault"
    grep -n 'halting' "$LOG" | head -10
    fail=1
fi

# --- boot milestones ------------------------------------------------------
check() {
    if grep -qF "$2" "$LOG"; then
        note "ok   $1"
    else
        echo "   MISS $1  (expected \"$2\")"
        fail=1
    fi
}

check "kernel entry"      "=== X OS ==="
check "interrupts up"     "gdt/idt/timer up"
check "scheduler + ipc"   "scheduler + ipc up"
check "ring-3 init"       "spawning ring-3 init"
check "init running"      "[init] start"

if [ "$fail" -eq 0 ]; then
    echo ">> boottest PASSED"
else
    echo ">> boottest FAILED"
fi
exit "$fail"
