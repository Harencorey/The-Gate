#!/bin/bash
# Run a test binary. On failure, re-run it under lldb for a backtrace and publish the
# tail of the output as an error annotation (readable through the REST API, unlike raw logs).
set -uo pipefail
BIN="$1"
OUT="$("$BIN" 2>&1)"; STATUS=$?
echo "$OUT"
if [ $STATUS -ne 0 ]; then
    BT="$(lldb --batch -o run -k "bt 25" -k quit -- "$BIN" 2>&1 | grep -E 'frame #|stop reason|FAIL' | head -40)"
    MSG="$(printf '%s\n--- backtrace ---\n%s' "$(echo "$OUT" | tail -25)" "$BT")"
    MSG="${MSG//'%'/'%25'}"; MSG="${MSG//$'\r'/}"; MSG="${MSG//$'\n'/'%0A'}"
    echo "::error title=$(basename "$BIN") failed (exit $STATUS)::$MSG"
fi
exit $STATUS
