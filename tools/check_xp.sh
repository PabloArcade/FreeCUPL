#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Check that Windows executables run on XP SP3: PE32 i386, OS and
# subsystem version 5.01 or lower, imports only from kernel32.dll and
# msvcrt.dll and on tools/xp_allowlist.txt, no SSE instructions.  An
# executable of the Windows GUI subsystem may also import what is on
# tools/xp_allowlist_gui.txt (user32, gdi32, comdlg32, comctl32, shell32).
# usage: tools/check_xp.sh file.exe ...
#        tools/check_xp.sh --selftest    (needs the mingw compiler, $WINCC)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ALLOW="$ROOT/tools/xp_allowlist.txt"
ALLOWGUI="$ROOT/tools/xp_allowlist_gui.txt"
if [ -z "$OBJDUMP" ]; then
  OBJDUMP=objdump
  command -v i686-w64-mingw32-objdump >/dev/null 2>&1 && OBJDUMP=i686-w64-mingw32-objdump
fi
TMP="$(mktemp -d "${TMPDIR:-/tmp}/check_xp.XXXXXX")" || exit 2
trap 'rm -rf "$TMP"' EXIT
trap 'exit 2' HUP INT TERM

# Import entries of "objdump -p": the lines of a "DLL Name:" block.  The
# columns before the name (vma, hint/ordinal) and after it (bound-to)
# vary between binutils versions; the name is the last field that is not
# a number, an address or "<none>".
imports() {
  awk '
    /DLL Name:/ { dll = $NF; n = 0; next }
    dll == "" { next }
    /^[ \t]*$/ { if (n) dll = ""; next }
    /^[^ \t]/ { dll = ""; next }
    $1 ~ /^vma:?$/ || /Member-Name|Hint\/Ord/ { next }
    {
      name = ""
      for (i = NF; i >= 2; i--) {
        f = $i
        if (f == "<none>" || f ~ /^[0-9]+$/ || f ~ /^0[xX][0-9a-fA-F]+$/ ||
            (f ~ /^[0-9a-fA-F]+$/ && f ~ /[0-9]/)) continue
        name = f
        break
      }
      if (name == "") name = "<ordinal>"
      print dll, name
      n++
    }' "$1"
}

field() {
  awk -v k="$2" '$1 == k { print $2; exit }' "$1"
}

check() {
  exe="$1"
  hdr="$TMP/hdr"
  if ! "$OBJDUMP" -p "$exe" > "$hdr" 2>/dev/null; then
    echo "FAIL $exe: not a PE file"
    return 1
  fi
  bad=""
  grep "file format pei-i386" "$hdr" > /dev/null || bad="$bad not-pei-i386"
  for k in OSystem Subsystem; do
    maj=$(field "$hdr" "Major${k}Version")
    min=$(field "$hdr" "Minor${k}Version")
    case "$maj" in ''|*[!0-9]*) bad="$bad Major${k}Version=$maj"; continue ;; esac
    case "$min" in ''|*[!0-9]*) bad="$bad Minor${k}Version=$min"; continue ;; esac
    if [ "$maj" -gt 5 ]; then bad="$bad Major${k}Version=$maj"
    elif [ "$maj" -eq 5 ] && [ "$min" -gt 1 ]; then bad="$bad Minor${k}Version=$min"
    fi
  done
  imports "$hdr" > "$TMP/imp"
  if awk '$1 == "Subsystem" && $2 + 0 == 2 { f = 1 } END { exit !f }' "$hdr"; then
    cat "$ALLOW" "$ALLOWGUI" > "$TMP/allow"
  else
    cat "$ALLOW" > "$TMP/allow"
  fi
  nimp=$(wc -l < "$TMP/imp")
  [ "$nimp" -gt 0 ] || bad="$bad no-imports-found"
  awk '
    NR == FNR {
      if ($1 ~ /^#/ || NF == 0) next
      if ($1 == "dll") { cur = tolower($2); dlls[cur] = 1; next }
      for (i = 1; i <= NF; i++) ok[cur SUBSEP $i] = 1
      next
    }
    {
      d = tolower($1)
      if (!(d in dlls)) { print "  disallowed DLL " $1; next }
      if (!((d SUBSEP $2) in ok)) print "  " $1 "!" $2 " not on XP allowlist"
    }' "$TMP/allow" "$TMP/imp" | sort -u > "$TMP/impbad"
  if [ -s "$TMP/impbad" ]; then
    bad="$bad imports"
    cat "$TMP/impbad"
  fi
  "$OBJDUMP" -d "$exe" > "$TMP/dis" 2>/dev/null
  n=$(grep -c '%[xy]mm' "$TMP/dis")
  [ "$n" -eq 0 ] || bad="$bad sse-instructions($n)"
  if [ -n "$bad" ]; then
    echo "FAIL $exe:$bad"
    return 1
  fi
  echo "ok   $exe ($nimp imports)"
  return 0
}

# Build small executables that must pass or fail, and run the check on
# them with objdump's own layout and with a rewritten one (other column
# widths, a bound-to column, blanks instead of tabs).
selftest() {
  cc="${WINCC:-i686-w64-mingw32-gcc}"
  command -v "$cc" >/dev/null 2>&1 || { echo "selftest: $cc not found" >&2; return 2; }
  d="$TMP/st"
  mkdir -p "$d"
  xp="-static -static-libgcc -Wl,--subsystem,console:5.01 -Wl,--major-os-version,5 -Wl,--minor-os-version,1 -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1"
  cat > "$d/good.c" <<'EOF'
#include <stdio.h>
int main(void) { puts("x"); return 0; }
EOF
  cat > "$d/vista.c" <<'EOF'
#include <windows.h>
#include <stdio.h>
int main(void) { printf("%lu\n", (unsigned long)GetTickCount64()); return 0; }
EOF
  cat > "$d/user32.c" <<'EOF'
#include <windows.h>
int main(void) { return MessageBoxA(0, "x", "x", 0); }
EOF
  cat > "$d/guinew.c" <<'EOF'
#include <windows.h>
int main(void) { SetProcessDPIAware(); return MessageBoxA(0, "x", "x", 0); }
EOF
  xpg="-static -static-libgcc -mwindows -Wl,--subsystem,windows:5.01 -Wl,--major-os-version,5 -Wl,--minor-os-version,1 -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1"
  cat > "$d/sse.c" <<'EOF'
int main(void) { __asm__ volatile ("xorps %%xmm0, %%xmm0" ::: "xmm0"); return 0; }
EOF
  set -- \
    "good    pass $d/good.c $xp" \
    "vista   import $d/vista.c -D_WIN32_WINNT=0x0600 $xp" \
    "user32  dll $d/user32.c $xp -luser32" \
    "gui     pass $d/user32.c $xpg" \
    "guinew  import $d/guinew.c -D_WIN32_WINNT=0x0600 $xpg" \
    "subsys  version $d/good.c -static -static-libgcc -Wl,--major-os-version,6 -Wl,--minor-os-version,0 -Wl,--major-subsystem-version,6 -Wl,--minor-subsystem-version,0" \
    "sse     sse $d/sse.c -msse $xp"
  cat > "$d/objdump2" <<EOF
#!/bin/sh
# objdump with another import table layout: 8-digit vma, blanks, bound-to
if [ "\$1" = "-p" ]; then
  "$OBJDUMP" "\$@" > "$d/raw" || exit 1
  awk '/DLL Name:/ { inb = 1 } /^[ \t]*\$/ { inb = 0 }
       inb && \$1 ~ /^[0-9a-f]+\$/ && NF >= 3 { printf "        %08x       %5d  %s  <none>\n", strtonum("0x" \$1), \$2, \$3; next }
       { print }' "$d/raw"
else
  exec "$OBJDUMP" "\$@"
fi
EOF
  # strtonum is gawk only: fall back to the vma as written
  if ! awk 'BEGIN { exit !(strtonum("0x10") == 16) }' 2>/dev/null; then
    sed 's/%08x/%8s/; s/strtonum("0x" \$1)/$1/' "$d/objdump2" > "$d/o2" && mv "$d/o2" "$d/objdump2"
  fi
  chmod +x "$d/objdump2"
  fails=0
  for t in "$@"; do
    set -- $t
    name=$1 want=$2
    shift 2
    if ! "$cc" -O1 -o "$d/$name.exe" "$@" 2>"$d/$name.err"; then
      echo "selftest: cannot build $name"; cat "$d/$name.err"; fails=1; continue
    fi
    for od in "$OBJDUMP" "$d/objdump2"; do
      out=$(OBJDUMP="$od" sh "$0" "$d/$name.exe")
      rc=$?
      case "$want:$rc" in
        pass:0) r=ok ;;
        pass:*) r=FAIL ;;
        import:0|dll:0|version:0|sse:0) r=FAIL ;;
        import:*) case "$out" in *"not on XP allowlist"*) r=ok ;; *) r=FAIL ;; esac ;;
        dll:*) case "$out" in *"disallowed DLL"*) r=ok ;; *) r=FAIL ;; esac ;;
        version:*) case "$out" in *SubsystemVersion=*) r=ok ;; *) r=FAIL ;; esac ;;
        sse:*) case "$out" in *sse-instructions*) r=ok ;; *) r=FAIL ;; esac ;;
      esac
      case "$out" in *no-imports-found*) r=FAIL ;; esac
      echo "selftest $r: $name ($want) with $(basename "$od")"
      [ "$r" = ok ] || { echo "$out" | sed 's/^/    /'; fails=1; }
    done
  done
  [ "$fails" -eq 0 ] && echo "selftest: ok" || echo "selftest: FAILED"
  return $fails
}

[ $# -gt 0 ] || { echo "usage: $0 file.exe... | --selftest" >&2; exit 2; }
if [ "$1" = "--selftest" ]; then
  selftest
  exit $?
fi
status=0
for exe in "$@"; do
  check "$exe" || status=1
done
exit $status
