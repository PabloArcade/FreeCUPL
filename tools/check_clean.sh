#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Tree hygiene: no home paths and no forbidden names in tracked files.
# The name pattern uses character classes so this file passes too.
# usage: tools/check_clean.sh   (exit 1 on a hit)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2
fail=0
p1='/[h]ome/|\\[h]ome\\'
out=$(git grep -nE "$p1" -- . 2>/dev/null)
if [ -n "$out" ]; then
  echo "check_clean: home paths in tracked files:"; echo "$out" | head -20; fail=1
fi
p2='[c][l][a][u][d][e]|[a][n][t][h][r][o][p][i][c]'
out=$(git grep -niE "$p2" -- . 2>/dev/null)
if [ -n "$out" ]; then
  echo "check_clean: forbidden names in tracked files:"; echo "$out" | head -20; fail=1
fi
[ $fail = 0 ] && echo "check_clean: ok"
exit $fail
