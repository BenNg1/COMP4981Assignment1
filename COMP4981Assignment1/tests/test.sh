#!/usr/bin/env bash
set -euo pipefail

PORT=18080
DOCROOT=./www
SERVER=./http_server

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

$SERVER "$PORT" "$DOCROOT" > /tmp/http_server_test.log 2>&1 &
SERVER_PID=$!

sleep 0.5

echo "[1] GET existing file"
code=$(curl -s -o /tmp/get_body -w "%{http_code}" "http://127.0.0.1:${PORT}/index.html")
[[ "$code" == "200" ]]
cmp -s /tmp/get_body "$DOCROOT/index.html"
echo "  OK"

echo "[2] HEAD existing file"
headers=$(curl -s -I "http://127.0.0.1:${PORT}/index.html")
echo "$headers" | grep -q "HTTP/1.1 200"
python3 - <<'PY'
import socket
s=socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("127.0.0.1", 18080))
req=b"HEAD /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n"
s.sendall(req)
chunks=[]
while True:
    d=s.recv(4096)
    if not d:
        break
    chunks.append(d)
s.close()
resp=b"".join(chunks)
sep=b"\r\n\r\n"
idx=resp.find(sep)
assert idx!=-1, "missing header terminator"
body=resp[idx+len(sep):]
assert len(body)==0, f"HEAD returned body bytes={len(body)}"
PY
echo "  OK"

echo "[3] Unsupported method -> 405"
code=$(curl -s -o /dev/null -w "%{http_code}" -X PUT "http://127.0.0.1:${PORT}/index.html")
[[ "$code" == "405" ]]
echo "  OK"

echo "[4] Malformed request -> 400"
resp=$(python3 - <<'PY'
import socket
s=socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("127.0.0.1", 18080))
s.sendall(b"GET\r\n\r\n")
print(s.recv(256).decode("latin1", errors="replace").splitlines()[0])
s.close()
PY
)
echo "$resp" | grep -q "400"
echo "  OK"

echo "[5] Path traversal -> 403 or 404"
code=$(curl --path-as-is -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:${PORT}/../etc/passwd")
if [[ "$code" != "403" && "$code" != "404" ]]; then
  echo "Expected 403 or 404, got $code"
  exit 1
fi
echo "  OK"

echo "[6] 20 concurrent clients"
fail=0
while read -r c; do
  [[ "$c" == "200" ]] || fail=1
done < <(seq 1 20 | xargs -I{} -P20 curl -s -o /dev/null -w "%{http_code}\n" "http://127.0.0.1:${PORT}/index.html")
[[ "$fail" == "0" ]]
echo "  OK"

echo "All tests passed."
