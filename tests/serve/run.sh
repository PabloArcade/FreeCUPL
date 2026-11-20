#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests of freecupl serve with curl against the running server
# (make check-serve): the page, files, compile and simulate, messages,
# options, devices, and the requests that must be refused.
#
# usage: tests/serve/run.sh BINDIR
#   BINDIR  directory with freecupl and freecsim
# RUN=wine EXE=.exe tests the win32 programs.  Needs curl.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -eq 1 ] || { echo "usage: $0 BINDIR" >&2; exit 2; }
case "$1" in /*) BIN="$1" ;; *) BIN="$(pwd)/$1" ;; esac
RUN=${RUN:-}
EXE=${EXE:-}
command -v curl >/dev/null 2>&1 || { echo "serve: curl not found, not tested"; exit 0; }
[ -f "$BIN/freecupl$EXE" ] || { echo "serve: $BIN/freecupl$EXE not found (run make first)" >&2; exit 2; }
TMP="${TMPDIR:-/tmp}/freecupl-serve.$$"
rm -rf "$TMP"
mkdir -p "$TMP/work" "$TMP/out" || exit 2
W="$TMP/work"
PID=""
cleanup() {
  [ -n "$PID" ] && kill "$PID" 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 2' INT TERM

pass=0
fail=0
ok() {   # ok DESCRIPTION : the previous command succeeded
  if [ $? -eq 0 ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL $1"; fi
}
is() {   # is DESCRIPTION GOT WANT
  if [ "$2" = "$3" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL $1: got '$2', want '$3'"; fi
}
has() {  # has DESCRIPTION FILE PATTERN
  if grep -q -- "$3" "$2"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL $1: no '$3' in"; head -c 600 "$2"; echo; fi
}

cp "$ROOT/examples/counter4.pld" "$ROOT/examples/counter4.si" "$W/"
printf 'secret\n' > "$TMP/outside.txt"
printf 'x\n' > "$W/.hidden"
mkdir "$W/sub"
printf 'x\n' > "$W/sub/inner.pld"
[ -n "$RUN" ] || ln -s "$TMP/outside.txt" "$W/link.pld" 2>/dev/null

start() {  # start ARGS... : runs the server, sets PID URL PORT TOKEN B
  : > "$TMP/server.log"   # not the address of an earlier server
  FREECUPL_SERVE_INI="$TMP/serve.ini" $RUN "$BIN/freecupl$EXE" serve --no-browser "$@" > "$TMP/server.log" 2>&1 &
  PID=$!
  URL=""
  n=0
  while [ $n -lt 300 ]; do
    URL=$(tr -d '\r' < "$TMP/server.log" | sed -n 's/^Open //p')
    [ -n "$URL" ] && break
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.1
    n=$((n + 1))
  done
  [ -n "$URL" ] || { echo "serve: the server did not start"; cat "$TMP/server.log"; exit 1; }
  case "$URL" in *"/#t="*) ;; *) echo "serve: unexpected address $URL"; exit 1 ;; esac
  TOKEN=${URL##*=}
  B=${URL%%/#*}
  PORT=${B##*:}
}

# status of a request; the body goes to $TMP/out/body, the header to head
req() {
  curl -s -m 20 -o "$TMP/out/body" -D "$TMP/out/head" -w '%{http_code}' "$@"
}
get() {  # get PATH-AND-QUERY [CURL-OPTION...] : with the token
  p="$1"; shift
  req -H "X-FreeCUPL-Token: $TOKEN" "$@" "$B$p"
}
post() { # post PATH-AND-QUERY DATA-OPTION...
  p="$1"; shift
  req -X POST -H "X-FreeCUPL-Token: $TOKEN" "$@" "$B$p"
}
wait_run() {
  n=0
  while [ $n -lt 300 ]; do
    get "/api/job?from=0" > /dev/null
    grep -q '"active":0' "$TMP/out/body" && return 0
    sleep 0.1
    n=$((n + 1))
  done
  return 1
}

start "$W"

# ---- the address is on the loopback interface only ----
tr -d '\r' < "$TMP/server.log" > "$TMP/server.txt"
has "URL printed" "$TMP/server.txt" "^Open http://127\.0\.0\.1:[0-9]*/#t=[0-9a-f]\{32\}\$"
other=$( (hostname -I 2>/dev/null || ipconfig getifaddr en0 2>/dev/null) | tr ' ' '\n' | grep -v '^127\.' | grep '^[0-9.]*$' | head -1)
if [ -n "$other" ]; then
  curl -s -m 3 -o /dev/null "http://$other:$PORT/"
  [ $? -ne 0 ]; ok "no answer on $other"
fi

# ---- the page: no token needed, no secret in it, no cookie ----
is "page" "$(req "$B/")" 200
has "page is the editor" "$TMP/out/body" '<title>FreeCUPL</title>'
has "content security policy" "$TMP/out/head" "^Content-Security-Policy: default-src 'none'"
if grep -qi '^Access-Control' "$TMP/out/head"; then false; else true; fi; ok "no CORS header"
for f in "" app.js app.css kw.js favicon.ico; do
  is "/$f" "$(req "$B/$f")" 200
  if grep -q "$TOKEN" "$TMP/out/body" "$TMP/out/head"; then false; else true; fi; ok "/$f: no token in it"
  if grep -qi '^Set-Cookie' "$TMP/out/head"; then false; else true; fi; ok "/$f: no cookie"
done
req "$B/kw.js" > /dev/null; has "reserved words" "$TMP/out/body" '"pinnode"'

# ---- token: the header on every request under /api/, nothing else ----
for p in /api/info /api/files "/api/file?name=counter4.pld" "/api/outputs?name=counter4.pld" \
         /api/options /api/job /api/devices /api/guide /api/nosuch; do
  is "no token: $p" "$(req "$B$p")" 403
  if grep -q "$TOKEN" "$TMP/out/body" "$TMP/out/head"; then false; else true; fi; ok "no token: $p: nothing given away"
  case "$p" in *\?*) q="&" ;; *) q="?" ;; esac
  is "token in the address: $p" "$(req "$B$p${q}t=$TOKEN")" 403
  is "token in a cookie: $p" "$(req -H "Cookie: fct$PORT=$TOKEN; t=$TOKEN; fc-token=$TOKEN" "$B$p")" 403
  is "wrong token: $p" "$(req -H "X-FreeCUPL-Token: 00000000000000000000000000000000" "$B$p")" 403
done
for p in "/api/file?name=h.pld" /api/options "/api/prefs?tab=4" "/api/run?action=compile&name=counter4.pld" \
         /api/stop "/api/devstmt?mn=g16v8" /api/quit; do
  is "POST without token: $p" "$(req -X POST -d x "$B$p")" 403
  case "$p" in *\?*) q="&" ;; *) q="?" ;; esac
  is "POST with the token in address and cookie: $p" "$(req -X POST -d x -H "Cookie: fct$PORT=$TOKEN" "$B$p${q}t=$TOKEN")" 403
done
[ ! -e "$W/h.pld" ] && [ ! -e "$W/counter4.jed" ]; ok "nothing written or run without the header"
is "with the header" "$(get /api/files)" 200
if grep -qi '^Set-Cookie' "$TMP/out/head"; then false; else true; fi; ok "no cookie with the token either"
# no ticket was made with --no-browser
is "no ticket to trade" "$(req -X POST -d '' "$B/api/session?k=")" 403
is "no ticket to trade, any value" "$(req -X POST -d '' "$B/api/session?k=$TOKEN")" 403

# ---- Host and Origin ----
is "Host of another site" "$(get /api/files -H "Host: attacker.example:$PORT")" 403
is "Host of another site, no port" "$(get /api/files -H "Host: attacker.example")" 403
is "Host with the address as prefix" "$(get /api/files -H "Host: 127.0.0.1.attacker.example:$PORT")" 403
is "Host with another port" "$(get /api/files -H "Host: 127.0.0.1:1")" 403
is "Host localhost" "$(get /api/files -H "Host: localhost:$PORT")" 200
is "page under the Host of another site" "$(req -H "Host: attacker.example:$PORT" "$B/")" 403
is "script under the Host of another site" "$(req -H "Host: attacker.example:$PORT" "$B/app.js")" 403
is "Origin of another site" "$(get /api/files -H "Origin: http://attacker.example")" 403
is "Origin null" "$(get /api/files -H "Origin: null")" 403
is "own Origin" "$(get /api/files -H "Origin: http://127.0.0.1:$PORT")" 200
is "preflight" "$(req -X OPTIONS -H "Origin: http://attacker.example" -H "Access-Control-Request-Method: POST" "$B/api/quit")" 405

# ---- files ----
is "file list" "$(get /api/files)" 200
has "source listed" "$TMP/out/body" '"name":"counter4.pld","size":[0-9]*,"ro":0'
if grep -q 'hidden\|"sub"\|inner\|link.pld\|outside' "$TMP/out/body"; then false; else true; fi
ok "dot files, directories and links not listed"
is "open" "$(get "/api/file?name=counter4.pld")" 200
cmp -s "$TMP/out/body" "$W/counter4.pld"; ok "file content"
has "line ends reported" "$TMP/out/head" "^X-FreeCUPL-Eol: lf"
has "source is not read only" "$TMP/out/head" "^X-FreeCUPL-Readonly: 0"
is "missing file" "$(get "/api/file?name=nosuch.pld")" 404
is "info" "$(get /api/info)" 200
has "version" "$TMP/out/body" '"version":"FreeCUPL [0-9.]* (DARKSOFT)"'

# ---- path traversal ----
for n in '../outside.txt' '..%2Foutside.txt' '%2e%2e%2foutside.txt' '..%5Coutside.txt' \
         '/etc/passwd' '%2Fetc%2Fpasswd' 'sub/inner.pld' 'sub%2Finner.pld' '.hidden' '..' '.' '' \
         'a%00.pld' 'C:%5Cx.pld' 'x.pld:stream' 'nul' 'NUL.pld' 'com1.txt' 'a%0Ab.pld' '-j.pld' 'x.pld.' ; do
  is "read refused: $n" "$(get "/api/file?name=$n")" 400
  is "write refused: $n" "$(post "/api/file?name=$n" --data-binary 'x')" 400
done
is "no name" "$(get "/api/file")" 400
if [ -L "$W/link.pld" ]; then
  is "link out of the directory: read" "$(get "/api/file?name=link.pld")" 403
  is "link out of the directory: write" "$(post "/api/file?name=link.pld" --data-binary 'x')" 403
  is "outside file untouched" "$(cat "$TMP/outside.txt")" secret
fi
is "path outside the page" "$(req --path-as-is "$B/../../etc/passwd")" 404
is "path outside the page, encoded" "$(req --path-as-is "$B/%2e%2e/%2e%2e/etc/passwd")" 404
is "path outside the page, under /api/" "$(get "/api/../../etc/passwd" --path-as-is)" 404
is "directory as file" "$(get "/api/file?name=sub")" 403

# ---- size limits and malformed requests ----
dd if=/dev/zero bs=1024 count=4200 2>/dev/null | tr '\0' 'a' > "$TMP/big"
is "body over the limit" "$(post "/api/file?name=big.pld" --data-binary "@$TMP/big" -H 'Expect:')" 413
[ ! -e "$W/big.pld" ]; ok "nothing written for it"
long=$(dd if=/dev/zero bs=1024 count=9 2>/dev/null | tr '\0' 'a')
is "header over the limit" "$(get /api/files -H "X-Long: $long")" 431
is "address over the limit" "$(get "/api/files?x=$(printf '%s' "$long" | head -c 2000)")" 414
is "method not allowed" "$(get "/api/file?name=counter4.pld" -X DELETE)" 405
is "PUT" "$(get "/api/file?name=counter4.pld" -X PUT -d x)" 405
is "chunked body" "$(post "/api/file?name=c.pld" -H 'Transfer-Encoding: chunked' --data-binary x)" 501
is "HTTP/1.0 without Host" "$(get /api/files -0 -H 'Host:')" 400
is "unknown request" "$(get /api/nosuch)" 404

# ---- save ----
printf 'Name x;\r\nDevice g16v8;\r\n' > "$TMP/crlf"
is "save" "$(post "/api/file?name=new.pld" --data-binary "@$TMP/crlf")" 200
cmp -s "$TMP/crlf" "$W/new.pld"; ok "saved bytes"
get "/api/file?name=new.pld" > /dev/null
has "CR LF reported" "$TMP/out/head" "^X-FreeCUPL-Eol: crlf"
is "new=1 on an existing file" "$(post "/api/file?name=new.pld&new=1" --data-binary x)" 409
cmp -s "$TMP/crlf" "$W/new.pld"; ok "file kept"
is "save to an output file" "$(post "/api/file?name=counter4.jed" --data-binary x)" 403

# ---- compile ----
is "compile" "$(post "/api/run?action=compile&name=counter4.pld" -d '')" 200
wait_run; ok "compile ends"
has "command line" "$TMP/out/body" '"k":"cmd","line":0,"file":"","text":"freecupl -jxl counter4.pld"'
has "written file" "$TMP/out/body" '"k":"file","line":0,"file":"counter4.jed"'
has "summary" "$TMP/out/body" '"k":"summary","line":0,"file":"","text":"Compile: OK - 0 errors, 0 warnings"'
# the same as the command line
mkdir "$TMP/ref"
cp "$ROOT/examples/counter4.pld" "$ROOT/examples/counter4.si" "$TMP/ref/"
(cd "$TMP/ref" && $RUN "$BIN/freecupl$EXE" -jxl counter4.pld > /dev/null 2>&1)
fuses() { tr -d '\r' < "$1" | grep '^\*[LCQGF]'; }
[ "$(fuses "$W/counter4.jed")" = "$(fuses "$TMP/ref/counter4.jed")" ] && [ -n "$(fuses "$W/counter4.jed")" ]
ok "JEDEC file equal to the command line's"

# ---- viewers ----
is "viewer" "$(get "/api/file?name=counter4.jed")" 200
cmp -s "$TMP/out/body" "$W/counter4.jed"; ok "viewer content"
has "output is read only" "$TMP/out/head" "^X-FreeCUPL-Readonly: 1"
is "outputs of the design" "$(get "/api/outputs?name=counter4.si")" 200
has "names of the outputs" "$TMP/out/body" '"pld":"counter4.pld","si":"counter4.si","jed":"counter4.jed","doc":"counter4.doc","lst":"counter4.lst","so":""'
is "stat" "$(get "/api/file?name=counter4.jed&stat=1")" 200
has "time of the file" "$TMP/out/body" '"mtime":"[0-9]*\.[0-9]*"'
is "guide" "$(get /api/guide)" 200
has "guide text" "$TMP/out/body" 'FREECUPL SERVE'

# ---- simulate ----
is "simulate" "$(post "/api/run?action=simulate&name=counter4.si" -d '')" 200
wait_run; ok "simulate ends"
has "compile step" "$TMP/out/body" '"text":"freecupl -jxla counter4.pld"'
has "simulate step" "$TMP/out/body" '"text":"freecsim -l counter4.si"'
has "simulate summary" "$TMP/out/body" '"text":"Simulate: OK - 0 errors, 0 warnings"'
(cd "$TMP/ref" && $RUN "$BIN/freecupl$EXE" -jxla counter4.pld > /dev/null 2>&1 && $RUN "$BIN/freecsim$EXE" -l counter4.si > /dev/null 2>&1)
tr -d '\r' < "$W/counter4.so" | grep -v 'FreeCUPL\|Created\|[0-9][0-9]:[0-9][0-9]' > "$TMP/so1"
tr -d '\r' < "$TMP/ref/counter4.so" | grep -v 'FreeCUPL\|Created\|[0-9][0-9]:[0-9][0-9]' > "$TMP/so2"
cmp -s "$TMP/so1" "$TMP/so2" && [ -s "$TMP/so1" ]; ok "simulation listing equal to the command line's"
get "/api/job?from=3" > /dev/null
if grep -q '"k":"cmd","line":0,"file":"","text":"freecupl' "$TMP/out/body"; then false; else true; fi
ok "messages from an index"

# ---- errors ----
sed 's/^q0\.d = q0 \$ en;/q0.d = q0 $ $ en;/' "$ROOT/examples/counter4.pld" > "$TMP/bad.pld"
is "save a source with an error" "$(post "/api/file?name=bad.pld" --data-binary "@$TMP/bad.pld")" 200
is "compile it" "$(post "/api/run?action=compile&name=bad.pld" -d '')" 200
wait_run; ok "failed compile ends"
has "error with its line" "$TMP/out/body" '"k":"error","line":32,"file":"bad.pld","text":"\[0007ca\] line(32) invalid syntax'
has "source line shown" "$TMP/out/body" '"k":"context","line":32,"file":"bad.pld"'
has "failed summary" "$TMP/out/body" '"k":"abort","line":0,"file":"","text":"Compile: FAILED - 1 error, 0 warnings"'
# a failed vector: the message comes from the listing
sed 's/^C 0011 LLLH L/C 0011 LLHH L/' "$ROOT/examples/counter4.si" > "$TMP/counter4.si"
post "/api/file?name=counter4.si" --data-binary "@$TMP/counter4.si" > /dev/null
post "/api/run?action=simulate&name=counter4.pld" -d '' > /dev/null
wait_run; ok "failed simulation ends"
has "simulator message with the listing line" "$TMP/out/body" '"k":"error","line":[0-9]*,"file":"counter4.so","text":"counter4.so([0-9]*): \[0019sa\]'
has "failed simulation summary" "$TMP/out/body" '"text":"Simulate: FAILED - [0-9]* errors*, 0 warnings"'
is "no source" "$(post "/api/run?action=compile&name=nosuch.pld" -d '')" 404
has "no source text" "$TMP/out/body" '"error":"No source file nosuch.pld"'
is "no simulation input" "$(post "/api/run?action=simulate&name=new.pld" -d '')" 404
is "bad action" "$(post "/api/run?action=delete&name=new.pld" -d '')" 400

# ---- options ----
is "options" "$(get "/api/options?name=counter4.pld")" 200
has "default options" "$TMP/out/body" '"perfile":0,"opts":{"jed":1,"doc":1,"fuseplot":0,"lst":1,"abs":0,"mx":0,"namesrc":0,"secure":0,"kb":0,"min":1,"simafter":0,"compat":0,"simlatch":0,"simlst":1,"simjed":0,"device":""}'
is "set options for one file" "$(post "/api/options?name=counter4.pld&perfile=1" -d 'jed=1&doc=0&fuseplot=0&lst=0&abs=0&mx=1&namesrc=1&secure=1&kb=1&min=3&simafter=0&compat=1&simlatch=1&simlst=1&simjed=1&device=g22v10')" 200
get "/api/options?name=counter4.pld" > /dev/null
has "options of the file" "$TMP/out/body" '"perfile":1,"opts":{"jed":1,"doc":0,"fuseplot":0,"lst":0,"abs":0,"mx":1,"namesrc":1,"secure":1,"kb":1,"min":3,"simafter":0,"compat":1,"simlatch":1,"simlst":1,"simjed":1,"device":"g22v10"}'
get "/api/options?name=new.pld" > /dev/null
has "other files keep the common options" "$TMP/out/body" '"perfile":0,"opts":{"jed":1,"doc":1'
cp "$ROOT/examples/counter4.si" "$W/counter4.si"
post "/api/run?action=simulate&name=counter4.pld" -d '' > /dev/null
wait_run; ok "run with options ends"
has "compiler flags from the options" "$TMP/out/body" '"text":"freecupl -jeang -m3 -kb -compat-refdefect g22v10 counter4.pld"'
has "simulator flags from the options" "$TMP/out/body" '"text":"freecsim -ljn -compat-refdefect -simlatch counter4.si"'
is "unknown device refused" "$(post "/api/options" -d 'device=-x')" 400
is "back to the common options" "$(post "/api/options?name=counter4.pld&perfile=0" -d 'jed=1&doc=1&lst=1&min=1&simlst=1')" 200
get "/api/options?name=counter4.pld" > /dev/null
has "common options again" "$TMP/out/body" '"perfile":0,"opts":{"jed":1,"doc":1,"fuseplot":0,"lst":1'
has "settings file" "$TMP/serve.ini" '^\[options\]'

# ---- devices ----
is "device list" "$(get /api/devices)" 200
nd=$($RUN "$BIN/freecupl$EXE" -devices 2>/dev/null | tr -d '\r' | grep -c .)
has "all devices" "$TMP/out/body" "^{\"total\":$nd,"
is "as many entries" "$(tr '[' '\n' < "$TMP/out/body" | grep -c '^"')" "$nd"
is "device search" "$(get "/api/devices?q=16v8+complex")" 200
has "search result" "$TMP/out/body" '\["g16v8ma",20,"GAL16V8 (complex mode)"\]'
if grep -q '"g22v10"' "$TMP/out/body"; then false; else true; fi; ok "search leaves the others out"
# sorted by the pins column: as numbers, and downwards the reverse
pins() { { tr '[' '\n' < "$TMP/out/body"; echo; } | sed -n 's/^"[^"]*",\([0-9]*\),.*/\1/p'; }
is "devices by pins" "$(get "/api/devices?s=1")" 200
pins > "$TMP/pins.up"
is "all of them" "$(grep -c . "$TMP/pins.up")" "$nd"
sort -n "$TMP/pins.up" | cmp -s - "$TMP/pins.up"; ok "pins upwards, as numbers"
is "more than one pin count" "$([ "$(head -1 "$TMP/pins.up")" -lt "$(tail -1 "$TMP/pins.up")" ] && echo yes)" yes
is "devices by pins, downwards" "$(get "/api/devices?s=1&d=1")" 200
pins > "$TMP/pins.down"
sort -n -r "$TMP/pins.down" | cmp -s - "$TMP/pins.down"; ok "pins downwards"
is "Device statement" "$(post "/api/devstmt?mn=g16v8" --data-binary "@$ROOT/examples/counter4.pld")" 200
at=$(grep -b -o 'Device   g22v10;' "$ROOT/examples/counter4.pld" | cut -d: -f1)
has "place of the statement" "$TMP/out/body" "{\"found\":1,\"start\":$at,\"end\":$((at + 16)),\"text\":\"Device   g16v8 ;\"}"
is "Device statement of an unknown device" "$(post "/api/devstmt?mn=nosuch" --data-binary x)" 400

# ---- outputs stay in the directory, whatever the Name item says ----
sed 's/^Name .*/Name     ..\/..\/escaped;/' "$ROOT/examples/counter4.pld" > "$TMP/esc.pld"
post "/api/file?name=esc.pld" --data-binary "@$TMP/esc.pld" > /dev/null
is "compile a source whose Name has a directory" "$(post "/api/run?action=compile&name=esc.pld" -d '')" 200
wait_run; ok "it ends"
has "the JEDEC file without the directory part" "$TMP/out/body" '"k":"file","line":0,"file":"escaped.jed"'
[ -f "$W/escaped.jed" ] && [ "$(find "$TMP" -name 'escaped*' | grep -c .)" -eq 1 ] && [ ! -e "$TMP/../escaped.jed" ]
ok "nothing written outside the directory"
# a Name that is no file name on Windows: refused, and the page is told
sed 's/^Name .*/Name     a*b;/' "$ROOT/examples/counter4.pld" > "$TMP/star.pld"
post "/api/file?name=star.pld" --data-binary "@$TMP/star.pld" > /dev/null
is "compile a source whose Name is no file name" "$(post "/api/run?action=compile&name=star.pld" -d '')" 200
wait_run; ok "it ends"
has "message 0001cc" "$TMP/out/body" '"k":"error","line":0,"file":"","text":"\[0001cc\] could not open:  a\*b.jed"'
[ -z "$(find "$W" -name 'a\*b*')" ]; ok "no file of that name"
# a source name much longer than its Name item
sed 's/^Name .*/Name     a;/' "$ROOT/examples/counter4.pld" > "$TMP/long.pld"
post "/api/file?name=a_source_with_a_rather_long_file_name_0123456789.pld" --data-binary "@$TMP/long.pld" > /dev/null
post "/api/run?action=compile&name=a_source_with_a_rather_long_file_name_0123456789.pld" -d '' > /dev/null
wait_run; ok "long source name: ends"
has "long source name: compiled" "$TMP/out/body" '"text":"Compile: OK - 0 errors, 0 warnings"'
[ -f "$W/a_source_with_a_rather_long_file_name_0123456789.lst" ] && [ -f "$W/a.jed" ]; ok "long source name: listing and JEDEC file"

# ---- connections that say nothing do not keep requests out ----
if curl -V | grep -q telnet; then
  n=0
  while [ $n -lt 40 ]; do
    (sleep 8 | curl -s -m 8 "telnet://127.0.0.1:$PORT" > /dev/null 2>&1 &)
    n=$((n + 1))
  done
  sleep 1
  is "request beside 40 silent connections" "$(curl -s -m 4 -o /dev/null -w '%{http_code}' -H "X-FreeCUPL-Token: $TOKEN" "$B/api/info")" 200
  is "and another" "$(curl -s -m 4 -o /dev/null -w '%{http_code}' -H "X-FreeCUPL-Token: $TOKEN" "$B/api/files")" 200
fi
# refused before the body is read
head -c 1000000 "$TMP/big" > "$TMP/mid"
is "large body without token" "$(req -X POST --data-binary "@$TMP/mid" -H 'Expect:' "$B/api/file?name=mid.pld")" 403
[ ! -e "$W/mid.pld" ]; ok "nothing written for it"

# ---- stop, then the end ----
is "stop with nothing running" "$(post /api/stop -d '')" 200
is "quit" "$(post /api/quit -d '')" 200
n=0
while kill -0 "$PID" 2>/dev/null && [ $n -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
if kill -0 "$PID" 2>/dev/null; then false; else true; fi; ok "server ended by quit"
wait "$PID"; is "exit status after quit" $? 0
PID=""

# Ctrl+C, and a port given on the command line
if [ -z "$RUN" ]; then
  start --port "$PORT" "$W"
  is "port from the command line" "${B##*:}" "$PORT"
  is "answers there" "$(get /api/info)" 200
  kill -INT "$PID"
  n=0
  while kill -0 "$PID" 2>/dev/null && [ $n -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
  wait "$PID"; is "exit status after an interrupt" $? 0
  has "says that it ended" "$TMP/server.log" '^freecupl serve: ended'
  PID=""
  FREECUPL_SERVE_INI="$TMP/serve.ini" "$BIN/freecupl" serve --no-browser "$TMP/nosuchdir" > "$TMP/out/body" 2>&1
  is "missing directory" $? 1

  # the browser is started with a ticket for one request, not the token
  mkdir "$TMP/fakebin"
  for b in xdg-open open; do
    printf '#!/bin/sh\nprintf "%%s\\n" "$1" > "%s/opened"\n' "$TMP" > "$TMP/fakebin/$b"
    chmod +x "$TMP/fakebin/$b"
  done
  FREECUPL_SERVE_INI="$TMP/serve.ini" PATH="$TMP/fakebin:$PATH" "$BIN/freecupl" serve "$W" > "$TMP/server.log" 2>&1 &
  PID=$!
  n=0
  while [ ! -s "$TMP/opened" ] && [ $n -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
  URL=$(sed -n 's/^Open //p' "$TMP/server.log"); TOKEN=${URL##*=}; B=${URL%%/#*}
  OPENED=$(cat "$TMP/opened" 2>/dev/null)
  TICKET=${OPENED##*=}
  case "$OPENED" in "$B/#k="*) true ;; *) false ;; esac; ok "browser started with a ticket ($OPENED)"
  case "$OPENED" in *"$TOKEN"*) false ;; *) true ;; esac; ok "the token is not in its arguments"
  is "ticket as token" "$(req -H "X-FreeCUPL-Token: $TICKET" "$B/api/files")" 403
  is "ticket in the address of another request" "$(req "$B/api/files?k=$TICKET")" 403
  is "ticket with GET" "$(req "$B/api/session?k=$TICKET")" 403
  is "wrong ticket" "$(req -X POST -d '' "$B/api/session?k=00000000000000000000000000000000")" 403
  is "ticket from the page of another site" "$(req -X POST -d '' -H "Origin: http://attacker.example" "$B/api/session?k=$TICKET")" 403
  is "ticket under another Host" "$(req -X POST -d '' -H "Host: attacker.example:${B##*:}" "$B/api/session?k=$TICKET")" 403
  is "ticket traded for the token" "$(req -X POST -d '' "$B/api/session?k=$TICKET")" 200
  has "the token" "$TMP/out/body" "^{\"token\":\"$TOKEN\"}\$"
  if grep -qi '^Set-Cookie' "$TMP/out/head"; then false; else true; fi; ok "no cookie with it"
  is "ticket a second time" "$(req -X POST -d '' "$B/api/session?k=$TICKET")" 403
  if grep -q "$TOKEN" "$TMP/out/body"; then false; else true; fi; ok "no token the second time"
  is "token still good" "$(get /api/info)" 200
  post /api/quit -d '' > /dev/null
  wait "$PID"; PID=""
fi

echo "serve: $pass passed, $fail failed"
[ $fail -eq 0 ]
