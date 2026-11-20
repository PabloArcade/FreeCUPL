#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Command-line options (part of make check): the minimisation levels on
# a design where they differ, the second attempt with shorter sums when
# the product terms do not fit, and the security fuse.
#
# decode.pld: at -m1 (default) the sums are those of WinCUPL's quick
# minimisation, with a literal that -m2 to -m4 remove; the levels give
# equivalent fuse maps.  full.pld: the quick minimisation needs three
# product terms where the device has two; the default mode then uses a
# shorter sum and says so (0909cb), the WinCUPL-exact mode rejects the
# design as cupl.exe does (0006cc).
#
# usage: tests/cli/run.sh BINDIR
# RUN=wine EXE=.exe tests the win32 programs.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -eq 1 ] || { echo "usage: $0 BINDIR" >&2; exit 2; }
case "$1" in /*) BIN="$1" ;; *) BIN="$(pwd)/$1" ;; esac
RUN=${RUN:-}
EXE=${EXE:-}
[ -f "$BIN/freecupl$EXE" ] || { echo "cli: $BIN/freecupl$EXE not found (run make first)" >&2; exit 2; }
TMP="${TMPDIR:-/tmp}/freecupl-cli.$$"
rm -rf "$TMP"
mkdir -p "$TMP" || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM
pass=0
fail=0
cp "$ROOT/tests/cli/decode.pld" "$ROOT/tests/cli/full.pld" "$TMP/"

ok() {   # ok DESCRIPTION : the previous command succeeded
  if [ $? -eq 0 ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL $1"; fi
}
is() {   # is DESCRIPTION GOT WANT
  if [ "$2" = "$3" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL $1: got '$2', want '$3'"; fi
}
fc() {   # fc ARGS... : freecupl in $TMP, messages in $TMP/log, status in RC
  (cd "$TMP" && $RUN "$BIN/freecupl$EXE" "$@" > log 2>&1 < /dev/null)
  RC=$?
  tr -d '\r' < "$TMP/log" > "$TMP/log.txt"
}
# the product terms of one equation in a .doc file, on one line
terms() {  # terms FILE NAME
  tr -d '\r' < "$TMP/$1" |
    awk -v n="$2" '/Expanded Product Terms/ { e = 1 } /Symbol Table/ { e = 0 }
      e && $0 ~ ("^!?" n " *=>") { p = 1; next } p && /^ *$/ { p = 0 }
      p { gsub(/^ +/, ""); gsub(/ +$/, ""); printf "%s%s", s, $0; s = " " }'
}
codes() { grep -o '^\[[0-9][0-9][0-9][0-9][a-z][a-z]\]' "$TMP/log.txt" | sort -u | tr -d '\n'; }
fuses() { $RUN "$BIN/jedtool$EXE" fuses "$TMP/$1" | tr -d '\r'; }

# ---- minimisation levels ----
fc -fix -jxn decode.pld
is "decode -m1: exit status" "$RC" 0
is "decode -m1: no message" "$(codes)" ""
is "decode -m1: sel" "$(terms decode.doc sel)" "!a0 & !strobe # a1 & a0 & !strobe"
is "decode -m1: ack" "$(terms decode.doc ack)" "!a1 & !strobe & ready # a1 & a0 & !strobe & ready"
cp "$TMP/decode.jed" "$TMP/m1.jed"
fc -fix -jxnm1 decode.pld
fuses decode.jed > "$TMP/f.a"; fuses m1.jed > "$TMP/f.b"
cmp -s "$TMP/f.a" "$TMP/f.b"; ok "decode: -m1 is the default"
fc -compat-refdefect -jxn decode.pld
fuses decode.jed > "$TMP/f.a"
cmp -s "$TMP/f.a" "$TMP/f.b"; ok "decode: the same fuse map in the WinCUPL-exact mode"
for m in 2 3 4; do
  fc -fix -jxnm$m decode.pld
  is "decode -m$m: exit status" "$RC" 0
  is "decode -m$m: sel" "$(terms decode.doc sel)" "!a0 & !strobe # a1 & !strobe"
  is "decode -m$m: ack" "$(terms decode.doc ack)" "!a1 & !strobe & ready # a0 & !strobe & ready"
  $RUN "$BIN/equiv$EXE" -q "$TMP/m1.jed" "$TMP/decode.jed" > /dev/null 2>&1; ok "decode -m$m: equivalent to -m1"
done
fc -fix -jxnm0 decode.pld
is "decode -m0: sel not minimised" "$(terms decode.doc sel)" "!a1 & !a0 & !strobe # a1 & !a0 & !strobe # a1 & a0 & !strobe"
$RUN "$BIN/equiv$EXE" -q "$TMP/m1.jed" "$TMP/decode.jed" > /dev/null 2>&1; ok "decode -m0: equivalent to -m1"

# ---- product terms that do not fit ----
fc -compat-refdefect -jxn full.pld
is "full, WinCUPL-exact: rejected" "$RC" 1
is "full, WinCUPL-exact: 0006cc" "$(codes)" "[0006cc]"
[ ! -f "$TMP/full.jed" ]; ok "full, WinCUPL-exact: no JEDEC file"
fc -fix -jxn full.pld
is "full: compiled" "$RC" 0
is "full: 0909cb" "$(codes)" "[0909cb]"
is "full: two product terms" "$(terms full.doc y)" "!a & b # a & c"
cp "$TMP/full.jed" "$TMP/full1.jed"
for m in 2 3 4; do
  fc -fix -jxnm$m full.pld
  is "full -m$m: compiled" "$RC" 0
  is "full -m$m: no message" "$(codes)" ""
  $RUN "$BIN/equiv$EXE" -q "$TMP/full1.jed" "$TMP/full.jed" > /dev/null 2>&1; ok "full -m$m: equivalent to the default"
done
rm -f "$TMP/full.jed"
fc -fix -jxnm0 full.pld
is "full -m0: rejected, no minimisation asked for" "$RC" 1
is "full -m0: 0006cc" "$(codes)" "[0006cc]"

# ---- security fuse ----
gfield() { tr -d '\r' < "$TMP/$1" | tr '*' '\n' | sed -n 's/^[ 	]*\(G[01]\)[ 	]*$/\1/p' | tr -d '\n'; }
fc -fix -jn decode.pld
is "no -g: G0" "$(gfield decode.jed)" G0
fuses decode.jed > "$TMP/f.a"
fc -fix -jng decode.pld
is "-g: exit status" "$RC" 0
is "-g: G1" "$(gfield decode.jed)" G1
fuses decode.jed > "$TMP/f.b"
cmp -s "$TMP/f.a" "$TMP/f.b"; ok "-g: the fuses are the same"
$RUN "$BIN/jedtool$EXE" check "$TMP/decode.jed" > /dev/null 2>&1; ok "-g: checksums"
fc -fix -jgn -m2 decode.pld
is "-g with other flags: G1" "$(gfield decode.jed)" G1
fc -compat-refdefect -jn decode.pld
is "WinCUPL-exact, no -g: G0" "$(gfield decode.jed)" G0
fc -compat-refdefect -jgn decode.pld
is "WinCUPL-exact, -g: G1" "$(gfield decode.jed)" G1

echo "cli: $pass passed, $fail failed"
[ $fail -eq 0 ]
