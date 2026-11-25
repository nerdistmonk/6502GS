#!/bin/sh
# Black-box oracle check for error messages: assemble one source with
# merlin32 and asm816, then require the same first error line and exit 1.
#
# usage: run_errors.sh <asm816> <merlin32> <merlin-library-dir> <case.s>
#
# Contract: merlin32 prints "      => [Error] <text>" lines to stdout. asm816
# prints the same form to stderr, reporting only its first error.
set -eu

asm816=$1
merlin=$2
lib=$3
src=$4

stem=$(basename "$src" .s)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cp "$src" "$work/$stem.s"

set +e
(cd "$work" && "$merlin" "$lib" "$stem.s" >merlin.out 2>/dev/null </dev/null)
merlin_rc=$?
(cd "$work" && "$asm816" -m "$lib" "$stem.s" >ours.out 2>ours.err </dev/null)
ours_rc=$?
set -e

[ "$merlin_rc" -eq 1 ] || { echo "merlin exit $merlin_rc, expected 1" >&2; exit 1; }
[ "$ours_rc" -eq 1 ] || { echo "asm816 exit $ours_rc, expected 1" >&2; exit 1; }

grep -m1 '=> \[Error\]' "$work/merlin.out" >"$work/oracle.txt"
grep -m1 '=> \[Error\]' "$work/ours.err" >"$work/ours.txt"
cmp "$work/oracle.txt" "$work/ours.txt"
