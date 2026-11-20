#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# FreeCUPL on macOS: a double click opens the editor page in the browser.
# The designs are kept in the folder FreeCUPL of the home folder; change
# DIR to use another one.  Arguments go to "freecupl serve".  DARKSOFT.
HERE=$(cd "$(dirname "$0")" && pwd) || exit 1
DIR="$HOME/FreeCUPL"
# macOS holds back downloaded programs as long as they have this mark
xattr -dr com.apple.quarantine "$HERE/bin" 2>/dev/null
if [ ! -d "$DIR" ]; then
  mkdir -p "$DIR" || exit 1
  cp "$HERE"/examples/*.pld "$HERE"/examples/*.si "$DIR"/
  echo "Created the folder $DIR for the designs"
  echo "and copied the examples into it."
fi
exec "$HERE/bin/freecupl" serve "$@" "$DIR"
