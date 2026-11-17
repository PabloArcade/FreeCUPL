#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Golden tests (make check).
#
# usage: tests/golden/run.sh BINDIR [PATTERN...]
#   BINDIR   directory with freecupl, freecsim, equiv and jedtool
#   PATTERN  shell patterns on the case id, e.g. 'bugs/*'
# RUN=wine EXE=.exe runs the win32 tools; KEEP=1 keeps failed cases in
# build/check-failed.  DEFAULT_ARGS=-fix is for tools built with
# FIX_DEFAULT=0: it is added where a case gives neither -fix nor
# -compat-refdefect.
#
# MANIFEST lines: ID SOURCE ARGS EXIT CODES [EXPECT].  cases/SOURCE.pld
# (and .si) is compiled with ARGS (comma separated; '@freecsim,FLAGS' runs
# freecupl -ajn and then freecsim FLAGS, a mode flag in FLAGS goes to
# both).  The exit status and the set of message codes must match;
# cases/EXPECT.jed (EXPECT defaults to ID) must be equivalent (equiv) with
# the same test vectors, cases/EXPECT.so must be identical.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
G="$ROOT/tests/golden"
[ $# -ge 1 ] || { echo "usage: $0 BINDIR [PATTERN...]" >&2; exit 2; }
case "$1" in /*) BIN="$1" ;; *) BIN="$(pwd)/$1" ;; esac
shift
RUN=${RUN:-}
EXE=${EXE:-}
for t in freecupl freecsim equiv jedtool; do
  [ -f "$BIN/$t$EXE" ] || { echo "golden: $BIN/$t$EXE not found (run make first)" >&2; exit 2; }
done
TMP="${TMPDIR:-/tmp}/freecupl-check.$$"
rm -rf "$TMP"
mkdir -p "$TMP" || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM

# test vector fields of a JEDEC file, one per line
vectors() {
  tr -d '\r' < "$1" | tr '\n' ' ' | tr '*' '\n' | sed 's/^[ 	]*//' | grep -E '^(QV[0-9]|P |V[0-9])' || true
}

pass=0
fail=0
selected() {
  [ $# -eq 0 ] && return 0
  for p in "$@"; do
    # shellcheck disable=SC2254
    case "$ID" in $p) return 0 ;; esac
  done
  return 1
}

n=0
while read -r ID SRC ARGS EXP CODES REF; do
  case "$ID" in ''|'#'*) continue ;; esac
  [ -n "$REF" ] || REF=$ID
  case ",$ARGS," in
    *,-fix,*|*,-compat-refdefect,*) ;;
    *) [ -n "${DEFAULT_ARGS:-}" ] && ARGS="$ARGS,$DEFAULT_ARGS" ;;
  esac
  MODE=""
  case ",$ARGS," in
    *,-compat-refdefect,*) MODE=-compat-refdefect ;;
    *,-fix,*) MODE=-fix ;;
  esac
  selected "$@" || continue
  n=$((n + 1))
  W="$TMP/w$n"
  mkdir "$W"
  NAME=${SRC##*/}
  cp "$G/cases/$SRC.pld" "$W/"
  [ -f "$G/cases/$SRC.si" ] && cp "$G/cases/$SRC.si" "$W/"
  case "$ARGS" in
    @freecsim,*)
      ARGV=$(echo "${ARGS#@freecsim,}" | tr ',' ' ')
      # shellcheck disable=SC2086
      (cd "$W" && SOURCE_DATE_EPOCH=0 $RUN "$BIN/freecupl$EXE" $MODE -ajn "$NAME.pld" > out.txt 2>&1 < /dev/null &&
        SOURCE_DATE_EPOCH=0 $RUN "$BIN/freecsim$EXE" $ARGV "$NAME" >> out.txt 2>&1 < /dev/null; echo $? > rc) ;;
    *)
      ARGV=$(echo "$ARGS" | tr ',' ' ')
      # shellcheck disable=SC2086
      (cd "$W" && SOURCE_DATE_EPOCH=0 $RUN "$BIN/freecupl$EXE" $ARGV "$NAME.pld" > out.txt 2>&1 < /dev/null; echo $? > rc) ;;
  esac
  why=""
  rc=$(cat "$W/rc")
  [ "$rc" = "$EXP" ] || why="exit status $rc, expected $EXP"
  got=$(tr -d '\r' < "$W/out.txt" | grep -o '\[[0-9][0-9][0-9][0-9][a-z][a-z]\]' | tr -d '[]' | sort -u | paste -s -d , -)
  [ -n "$got" ] || got=-
  [ -z "$why" ] && [ "$got" != "$CODES" ] && why="message codes $got, expected $CODES"
  if [ -z "$why" ]; then
    if [ -f "$G/cases/$REF.jed" ]; then
      if [ ! -f "$W/$NAME.jed" ]; then
        why="no JEDEC file written"
      elif ! $RUN "$BIN/jedtool$EXE" check "$W/$NAME.jed" > "$W/check.txt" 2>&1 < /dev/null; then
        why="jedtool check: $(tail -1 "$W/check.txt")"
      elif ! $RUN "$BIN/equiv$EXE" -q "$G/cases/$REF.jed" "$W/$NAME.jed" > "$W/equiv.txt" 2>&1 < /dev/null; then
        $RUN "$BIN/equiv$EXE" "$G/cases/$REF.jed" "$W/$NAME.jed" > "$W/equiv.txt" 2>&1 < /dev/null
        why="not equivalent: $(tail -1 "$W/equiv.txt")"
      else
        vectors "$G/cases/$REF.jed" > "$W/v.exp"
        vectors "$W/$NAME.jed" > "$W/v.got"
        cmp -s "$W/v.exp" "$W/v.got" || why="JEDEC test vectors differ"
      fi
    elif [ -f "$W/$NAME.jed" ]; then
      why="JEDEC file written, none expected"
    fi
  fi
  if [ -z "$why" ] && [ -f "$G/cases/$REF.so" ]; then
    if [ ! -f "$W/$NAME.so" ]; then
      why="no simulation listing written"
    elif ! cmp -s "$G/cases/$REF.so" "$W/$NAME.so"; then
      why="simulation listing differs"
    fi
  fi
  if [ -z "$why" ]; then
    pass=$((pass + 1))
    rm -rf "$W"
  else
    fail=$((fail + 1))
    echo "FAIL $ID: $why"
    if [ "${KEEP:-0}" = 1 ]; then
      mkdir -p "$ROOT/build/check-failed"
      rm -rf "$ROOT/build/check-failed/$n"
      cp -r "$W" "$ROOT/build/check-failed/$n"
      echo "     kept in build/check-failed/$n"
    fi
    rm -rf "$W"
  fi
done < "$G/MANIFEST"

echo "golden: $pass passed, $fail failed ($n cases)"
[ "$n" -gt 0 ] || { echo "golden: no case selected" >&2; exit 1; }
[ "$fail" -eq 0 ]
