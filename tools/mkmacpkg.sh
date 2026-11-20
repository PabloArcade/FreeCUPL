#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The macOS package freecupl-VERSION-macos.zip: the programs of
# build/macos for Apple and Intel processors, signed ad hoc, the
# launcher FreeCUPL.command, documentation and examples.  Run on macOS
# after
#     make native CC="cc -arch arm64 -arch x86_64 -mmacosx-version-min=11.0"
# usage: tools/mkmacpkg.sh OUTDIR
# On another system it packs build/linux unsigned, to try the layout.
set -eu
[ $# -eq 1 ] || { echo "usage: $0 OUTDIR" >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
cd "$ROOT"
VER=$(sed -n 's/^#define FREECUPL_VERSION "\(.*\)"$/\1/p' src/version.h)
[ -n "$VER" ] || { echo "no version in src/version.h" >&2; exit 1; }
MAC=no
[ "$(uname -s)" = Darwin ] && MAC=yes
B=build/linux
[ $MAC = yes ] && B=build/macos
NAME="freecupl-$VER-macos"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/mkmacpkg.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
P="$TMP/$NAME"
mkdir -p "$P/bin" "$P/docs" "$P/man/man1" "$P/examples"
for t in freecupl freecsim jedsim equiv jedtool; do
  [ -f "$B/$t" ] || { echo "$B/$t not found (run make first)" >&2; exit 1; }
  cp "$B/$t" "$P/bin/$t"
  chmod 755 "$P/bin/$t"
  if [ $MAC = yes ]; then
    strip "$P/bin/$t"
    archs=$(lipo -archs "$P/bin/$t")
    case " $archs " in *" arm64 "*) ;; *) echo "$t: no arm64 code ($archs)" >&2; exit 1 ;; esac
    case " $archs " in *" x86_64 "*) ;; *) echo "$t: no x86_64 code ($archs)" >&2; exit 1 ;; esac
    codesign --force --sign - "$P/bin/$t"
    codesign --verify --strict "$P/bin/$t"
  fi
done
cp README.txt LICENSE CHANGELOG.txt "$P/"
cp docs/USERGUIDE.txt docs/BUGS.txt docs/SIMFMT.txt "$P/docs/"
cp man/*.1 "$P/man/man1/"
cp examples/* "$P/examples/"
cp macos/FreeCUPL.command "$P/FreeCUPL.command"
chmod 755 "$P/FreeCUPL.command"
"$P/bin/freecupl" -V | grep "FreeCUPL $VER " > /dev/null || { echo "freecupl -V does not say $VER" >&2; exit 1; }
rm -f "$OUT/$NAME.zip"
# -X: no extra fields; -y: links as links (there are none)
(cd "$TMP" && zip -q -r -X -y "$OUT/$NAME.zip" "$NAME")
echo "$OUT/$NAME.zip"
