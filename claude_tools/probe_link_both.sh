#!/bin/sh
# Link one link-file directory with merlin32 and asm816 side by side, then diff
# the output directories. The directory must hold the link file (lk) and its
# sources. Prints both return codes and "IDENTICAL" when outputs match.
#
# usage: probe_link_both.sh <case-dir> [flag]
# env:   MERLIN32 (default /root/merlin32/Source/merlin32)
#        MERLIN_LIB (default /root/merlin32/Library)
#        ASM816 (default <repo>/build/asm816)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
merlin=${MERLIN32:-/root/merlin32/Source/merlin32}
lib=${MERLIN_LIB:-/root/merlin32/Library}
asm816=${ASM816:-$here/../build/asm816}
src=$1
flag=${2:-}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/m" "$work/o"
cp -R "$src/." "$work/m/"
cp -R "$src/." "$work/o/"

(cd "$work/m" && "$merlin" $flag "$lib" lk >merlin.out 2>&1 </dev/null; echo "merlin rc=$?") || true
(cd "$work/o" && "$asm816" -m "$lib" $flag lk >ours.out 2>&1 </dev/null; echo "asm816 rc=$?") || true
rm -f "$work/m/error_output.txt"
diff -r "$work/m" "$work/o" -x merlin.out -x ours.out && echo "IDENTICAL output dirs" || true
