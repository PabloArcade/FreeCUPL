#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Output file names (part of make check): every file freecupl and
# freecsim write, with source names and Name items of 1 to 200
# characters in all pairings, with and without -n.  The JEDEC file is
# named after the Name item as cupl.exe does it (up to the first blank,
# without a directory or drive, 28 characters; names that are no file
# name on Windows are refused), every other file after the source, the
# simulator files after the .si file.  Run with tools built with -fsanitize=address this shows a
# name buffer that is too small.
#
# usage: tests/names/run.sh BINDIR
# RUN=wine EXE=.exe tests the win32 programs.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -eq 1 ] || { echo "usage: $0 BINDIR" >&2; exit 2; }
case "$1" in /*) BIN="$1" ;; *) BIN="$(pwd)/$1" ;; esac
RUN=${RUN:-}
EXE=${EXE:-}
[ -f "$BIN/freecupl$EXE" ] || { echo "names: $BIN/freecupl$EXE not found (run make first)" >&2; exit 2; }
TMP="${TMPDIR:-/tmp}/freecupl-names.$$"
rm -rf "$TMP"
mkdir -p "$TMP" || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM
pass=0
fail=0

name() {  # name LENGTH PREFIX : a file name of that length
  awk -v n="$1" -v p="$2" 'BEGIN { s = p; while (length(s) < n) s = s "0123456789"; print substr(s, 1, n) }'
}

# one case: source stem S, Name item N ("" for the Name of the example)
case_() {
  S="$1"; N="$2"; D="$TMP/c$((pass + fail))"
  mkdir "$D"
  if [ -n "$N" ]; then
    sed "s/^Name .*/Name     $N;/" "$ROOT/examples/counter4.pld" > "$D/$S.pld"
    sed "s/^Name .*/Name     $N;/" "$ROOT/examples/counter4.si" > "$D/$S.si"
    O=$(printf '%s' "$N" | cut -c1-28)
  else
    cp "$ROOT/examples/counter4.pld" "$D/$S.pld"
    cp "$ROOT/examples/counter4.si" "$D/$S.si"
    O=counter4
  fi
  what="source ${#S} characters, Name ${#O}"
  bad=""
  # the JEDEC file named after the Name item (28 characters of it),
  # the others after the source; simulation from the compiler
  (cd "$D" && $RUN "$BIN/freecupl$EXE" -jxflaes "$S.pld" > log1 2>&1) || bad="$bad freecupl-jxflaes"
  for f in "$S.lst" "$S.mx" "$O.jed" "$S.doc" "$S.abs" "$S.so" "$S.wo"; do
    [ -s "$D/$f" ] || bad="$bad no-$(echo "$f" | sed 's/.*\.//')"
  done
  grep -q 'V0001 ' "$D/$O.jed" 2>/dev/null || bad="$bad no-s-vectors"
  rm -f "$D/$S.so" "$D/$S.wo" "$D/$S.abs" "$D/$O.jed"
  # the simulator alone (it reads the absolute file named after the
  # .si file), vectors into the JEDEC file named after the Name item
  (cd "$D" && $RUN "$BIN/freecupl$EXE" -ja "$S.pld" > log2 2>&1) || bad="$bad freecupl-ja"
  [ -s "$D/$S.abs" ] || bad="$bad no-abs"
  (cd "$D" && $RUN "$BIN/freecsim$EXE" -lj "$S.si" >> log2 2>&1) || bad="$bad freecsim-lj"
  [ -s "$D/$S.so" ] && [ -s "$D/$S.wo" ] || bad="$bad no-so-wo"
  grep -q 'V0001 ' "$D/$O.jed" 2>/dev/null || bad="$bad no-vectors"
  # the same named after the source
  rm -f "$D"/*.jed "$D"/*.doc "$D"/*.abs "$D"/*.lst "$D"/*.mx "$D"/*.so "$D"/*.wo
  (cd "$D" && $RUN "$BIN/freecupl$EXE" -jxflaesn "$S.pld" > log3 2>&1) || bad="$bad freecupl-n"
  for e in lst mx jed doc abs so wo; do
    [ -s "$D/$S.$e" ] || bad="$bad no-n-$e"
  done
  (cd "$D" && $RUN "$BIN/freecsim$EXE" -ljn "$S.si" > log4 2>&1) || bad="$bad freecsim-ljn"
  grep -q 'V0001 ' "$D/$S.jed" 2>/dev/null || bad="$bad no-n-vectors"
  # a source without extension on the command line, and through a directory
  (cd "$TMP" && $RUN "$BIN/freecupl$EXE" -jxla "c$((pass + fail))/$S" > "$D/log5" 2>&1) || bad="$bad freecupl-dir"
  [ -s "$D/$O.jed" ] && [ -s "$D/$S.lst" ] || bad="$bad no-dir-outputs"
  if [ -z "$bad" ]; then
    pass=$((pass + 1))
    rm -rf "$D"
  else
    fail=$((fail + 1))
    echo "FAIL $what:$bad"
    cat "$D"/log? 2>/dev/null | grep -v '^$' | head -8
  fi
}

# one Name item: the JEDEC file it gives, or "-" when it is refused
# because it is no file name on Windows (message 0001cc, no file; with
# -n the design compiles).  The same on every system.
special() {
  N="$1"; O="$2"; D="$TMP/s$((pass + fail))"
  mkdir "$D" "$D/sub"
  awk -v n="$N" '/^Name / { print "Name     " n ";"; next } { print }' "$ROOT/examples/counter4.pld" > "$D/src1.pld"
  awk -v n="$N" '/^Name / { print "Name     " n ";"; next } { print }' "$ROOT/examples/counter4.si" > "$D/src1.si"
  bad=""
  if [ "$O" = - ]; then
    if (cd "$D" && $RUN "$BIN/freecupl$EXE" -jxa src1.pld > log1 2>&1); then bad="$bad accepted"; fi
    grep -q '^\[0001cc\] could not open: ' "$D/log1" || bad="$bad no-0001cc"
    [ -z "$(find "$D" -name '*.jed')" ] || bad="$bad jed-written"
    if (cd "$D" && $RUN "$BIN/freecupl$EXE" -a src1.pld > log2 2>&1 &&
        $RUN "$BIN/freecsim$EXE" -lj src1.si >> log2 2>&1); then bad="$bad freecsim-accepted"; fi
    grep -q '^\[0001sa\] could not open: ' "$D/log2" || bad="$bad no-0001sa"
    [ -z "$(find "$D" -name '*.jed')" ] || bad="$bad jed-written-by-freecsim"
    (cd "$D" && $RUN "$BIN/freecupl$EXE" -jxan src1.pld > log3 2>&1) || bad="$bad refused-with-n"
    [ -s "$D/src1.jed" ] || bad="$bad no-n-jed"
  else
    (cd "$D" && $RUN "$BIN/freecupl$EXE" -jxas src1.pld > log1 2>&1) || bad="$bad freecupl"
    for f in "$O.jed" src1.doc src1.abs src1.so; do
      [ -s "$D/$f" ] || bad="$bad no-$f"
    done
    [ "$(find "$D" -name '*.jed' | wc -l)" -eq 1 ] || bad="$bad other-jed"
    [ -z "$(ls "$D/sub")" ] || bad="$bad file-in-sub"
    rm -f "$D/$O.jed"
    (cd "$D" && $RUN "$BIN/freecupl$EXE" -ja src1.pld > log2 2>&1 &&
      $RUN "$BIN/freecsim$EXE" -lj src1.si >> log2 2>&1) || bad="$bad freecsim"
    grep -q 'V0001 ' "$D/$O.jed" 2>/dev/null || bad="$bad no-vectors"
  fi
  if [ -z "$bad" ]; then
    pass=$((pass + 1))
    rm -rf "$D"
  else
    fail=$((fail + 1))
    echo "FAIL Name $N:$bad"
    cat "$D"/log? 2>/dev/null | grep -v '^$' | head -8
  fi
}

special 'AB12-(C)' 'AB12-(C)'
special 'a.b.c' 'a.b.c'
special 'x.jed' 'x.jed'
special 'a+b=c,d' 'a+b=c,d'
special '[x]#1' '[x]#1'
special 'two words' 'two'
special 'sub/inner' 'inner'
special '../../outside' 'outside'
special 'sub\\inner2' 'inner2'
special 'c:drive' 'drive'
special 'a*b' -
special 'a?b' -
special 'a<b>c' -
special 'a|b' -
special 'a"b' -
special 'con' -
special 'NUL' -
special 'Lpt1.x' -
special 'com9' -
special 'com0' 'com0'
special 'console' 'console'

for sl in 1 3 8 9 40 120 200; do
  S=$(name "$sl" s)
  case_ "$S" ""
  for nl in 1 8 31 120 200; do
    case_ "$S" "$(name "$nl" n)"
  done
done

echo "names: $pass passed, $fail failed"
[ $fail -eq 0 ]
