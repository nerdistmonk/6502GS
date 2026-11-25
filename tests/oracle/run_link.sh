#!/bin/sh
# Black-box oracle check for link mode: link one case directory with merlin32
# and asm816, then require the same exit code and the same output files.
#
# usage: run_link.sh <asm816> <merlin32> <merlin-library-dir> <case-dir>
#
# The case directory holds a link file named lk plus its sources and any
# existing _FileInformation.txt. merlin32 writes error_output.txt on some
# errors; asm816 does not, so that file is ignored.
set -eu

asm816=$1
merlin=$2
lib=$3
src=$4

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/m" "$work/o"
cp "$src"/* "$work/m/"
cp "$src"/* "$work/o/"

set +e
(cd "$work/m" && "$merlin" "$lib" lk >"$work/merlin.out" 2>&1 </dev/null)
merlin_rc=$?
(cd "$work/o" && "$asm816" -m "$lib" lk >"$work/ours.out" 2>&1 </dev/null)
ours_rc=$?
set -e

[ "$merlin_rc" -eq "$ours_rc" ] || { echo "exit merlin=$merlin_rc asm816=$ours_rc" >&2; exit 1; }
rm -f "$work/m/error_output.txt"
diff -r "$work/m" "$work/o"
