#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Check that Windows executables run on XP SP3: PE32 i386, OS and
# subsystem version 5.01 or lower, imports only from kernel32.dll and
# msvcrt.dll and on tools/xp_allowlist.txt, no SSE instructions.
# usage: tools/check_xp.sh file.exe ...
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ALLOW="$ROOT/tools/xp_allowlist.txt"
OBJDUMP="${OBJDUMP:-objdump}"
command -v i686-w64-mingw32-objdump >/dev/null 2>&1 && OBJDUMP=i686-w64-mingw32-objdump
status=0
[ $# -gt 0 ] || { echo "usage: $0 file.exe..." >&2; exit 2; }
for exe in "$@"; do
  hdr="$($OBJDUMP -p "$exe" 2>/dev/null)" || { echo "FAIL $exe: not a PE file"; status=1; continue; }
  bad=""
  echo "$hdr" | grep -q "file format pei-i386" || bad="$bad not-pei-i386"
  for f in MajorOSystemVersion MajorSubsystemVersion; do
    v=$(echo "$hdr" | awk -v k="$f" '$1==k{print $2}')
    [ -n "$v" ] && [ "$v" -le 5 ] || bad="$bad $f=$v"
  done
  for f in MinorOSystemVersion MinorSubsystemVersion; do
    maj=$(echo "$hdr" | awk -v k="$(echo $f | sed s/Minor/Major/)" '$1==k{print $2}')
    v=$(echo "$hdr" | awk -v k="$f" '$1==k{print $2}')
    [ "$maj" -lt 5 ] 2>/dev/null || [ "$v" -le 1 ] || bad="$bad $f=$v"
  done
  # objdump -p: "DLL Name: X" followed by hint/name lines
  imports=$(echo "$hdr" | awk '
    /DLL Name:/ {dll=$3; next}
    dll!="" && /^\t[0-9a-f]+[ \t]+[0-9]+[ \t]+[A-Za-z_]/ {print dll" "$3; next}
    /^$/ {if (dll!="" && seen) dll=""; next}
    {seen=1}')
  [ -n "$imports" ] || bad="$bad no-imports-found"
  echo "$imports" | while read dll fn; do
    [ -n "$dll" ] || continue
    awk -v d="$dll" -v f="$fn" '
      BEGIN{IGNORECASE=1; ok=0; dllok=0}
      /^#/ {next}
      $1=="dll" {cur=tolower($2); if (cur==tolower(d)) dllok=1; next}
      cur==tolower(d) {for(i=1;i<=NF;i++) if ($i==f) ok=1}
      END{ if(!dllok) {print "  disallowed DLL " d; exit 1}
           if(!ok) {print "  " d "!" f " not on XP allowlist"; exit 1} }' "$ALLOW" || echo "IMPORTFAIL"
  done > /tmp/check_xp.$$ 2>&1
  if grep -q IMPORTFAIL /tmp/check_xp.$$; then
    bad="$bad imports"
    grep -v IMPORTFAIL /tmp/check_xp.$$ | sort -u
  fi
  rm -f /tmp/check_xp.$$
  n=$($OBJDUMP -d "$exe" | grep -c '%[xy]mm')
  [ "$n" -eq 0 ] || bad="$bad sse-instructions($n)"
  if [ -n "$bad" ]; then echo "FAIL $exe:$bad"; status=1; else echo "ok   $exe ($(echo "$imports" | wc -l) imports)"; fi
done
exit $status
