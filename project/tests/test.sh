#!/usr/bin/env bash
set -euo pipefail

BIN="$(cd "$(dirname "$0")/.." && pwd)/bin"
SOCK=/tmp/minidocker.sock
ROOTFS="${ROOTFS:-/tmp/minidocker/rootfs}"

cleanup() {
    if [ -f "$SOCK" ]; then
        for id in $("$BIN/minidocker" list 2>/dev/null | awk 'NR>1{print $1}'); do
            "$BIN/minidocker" kill "$id" >/dev/null 2>&1 || true
        done
    fi
    if [ -n "${DAEMON_PID:-}" ]; then kill "$DAEMON_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT

if [ ! -x "$BIN/minidockerd" ]; then
    echo "Build first: make" >&2; exit 1
fi
if [ ! -d "$ROOTFS" ]; then
    echo "Download rootfs first: scripts/download-alpine-rootfs.sh $ROOTFS" >&2; exit 1
fi

rm -f "$SOCK"
"$BIN/minidockerd" &
DAEMON_PID=$!
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.1; done

pass=0; fail=0
ok()   { echo "  PASS: $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL: $1"; fail=$((fail+1)); }

echo "== test: echo output =="
out=$("$BIN/minidocker" run --rootfs "$ROOTFS" /bin/echo hello)
[ "$out" = "hello" ] && ok "echo hello" || bad "echo hello: got '$out'"

echo "== test: exit code =="
set +e
"$BIN/minidocker" run --rootfs "$ROOTFS" /bin/sh -c "exit 42" >/dev/null
rc=$?
set -e
[ "$rc" = "42" ] && ok "exit code 42 (got $rc)" || bad "exit code: got $rc"

echo "== test: PID 1 inside =="
out=$("$BIN/minidocker" run --rootfs "$ROOTFS" /bin/sh -c 'echo $$')
[ "$out" = "1" ] && ok "PID 1" || bad "PID: got '$out'"

echo "== test: UTS isolation =="
out=$("$BIN/minidocker" run --rootfs "$ROOTFS" /bin/sh -c 'hostname')
[ "$out" = "minidocker" ] && ok "hostname isolated" || bad "hostname: got '$out'"

echo "== test: detached + list =="
id=$("$BIN/minidocker" run -d --rootfs "$ROOTFS" /bin/sleep 5)
"$BIN/minidocker" list | grep -q "$id" && ok "detached container listed" || bad "list"

echo "== test: logs =="
"$BIN/minidocker" run -d --rootfs "$ROOTFS" /bin/sh -c 'echo foo; sleep 1'
sleep 0.3
for cid in $("$BIN/minidocker" list | awk 'NR>1{print $1}'); do
    "$BIN/minidocker" logs "$cid" 2>/dev/null | grep -q foo && { ok "logs contain 'foo'"; break; }
done

echo "== test: kill =="
"$BIN/minidocker" kill "$id" >/dev/null
"$BIN/minidocker" list | grep -q "$id.*exited" && ok "kill terminates" || bad "kill"

echo "== test: attach =="
id=$("$BIN/minidocker" run -d -t --rootfs "$ROOTFS" /bin/sh)
sleep 0.3
# send "echo attached\n" then exit
( printf 'echo attached\nexit\n'; sleep 0.3 ) | timeout 5 "$BIN/minidocker" attach "$id" | grep attached >/dev/null \
    && ok "attach round-trip" || bad "attach"
"$BIN/minidocker" kill "$id" >/dev/null || true

echo "== test: overlayfs =="
out=$("$BIN/minidocker" run --overlay --rootfs "$ROOTFS" \
        /bin/sh -c 'echo layered > /root/test.txt && cat /root/test.txt' 2>&1)
if [ "$out" = "layered" ] && [ ! -e "$ROOTFS/root/test.txt" ]; then
    ok "overlay: write goes to upper layer"
else
    bad "overlay: got '$out', rootfs test.txt exists=$([ -e "$ROOTFS/root/test.txt" ] && echo yes || echo no)"
fi

echo "== test: loopback =="
out=$("$BIN/minidocker" run --rootfs "$ROOTFS" /bin/busybox ip link show lo 2>/dev/null | grep -c "UP")
[ "$out" -ge 1 ] && ok "loopback is UP" || bad "loopback is DOWN"

echo
echo "=== $pass passed, $fail failed ==="
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)