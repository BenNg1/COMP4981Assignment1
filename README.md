# COMP 4981 Assignment 1 — Minimal HTTP Server Package

This package contains only the files needed for submission:
- source (`src/*.c`)
- headers (`include/*.h`)
- `Makefile`
- sample web root (`www/`)
- tests (`tests/test.sh`)

No compiled artifacts are included (`.o`, `http_server`, `http_server_asan`).

## Build
```bash
make
```

## Run
```bash
./http_server 8080 ./www
```

## Test
```bash
make test
```

## Clean
```bash
make clean
```
## Manual test checklist for HTTP server (run on second client)
```
i send the response and i expect a code back.

# 1) GET works (200 + body)
curl -i http://127.0.0.1:8080/index.html

# 2) HEAD works (200 + headers, no body)
curl -I http://127.0.0.1:8080/index.html

# 3) Unsupported method -> 405
curl -i -X PUT http://127.0.0.1:8080/index.html

# 4) Malformed request -> 400
printf "BADREQUEST\r\n\r\n" | nc 127.0.0.1 8080

# 5) Path traversal blocked -> 403 or safe 404
curl --path-as-is -i http://127.0.0.1:8080/../etc/passwd

# 6) Not found -> 404
curl -i http://127.0.0.1:8080/nope.txt

```
