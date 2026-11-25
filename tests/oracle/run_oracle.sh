#!/bin/sh
# Black-box oracle check: assemble one source with merlin32 and asm816 and
# require identical flat binaries.
#
# usage: run_oracle.sh <asm816> <merlin32> <merlin-library-dir> <case.s>
#
# Contract: asm816 is invoked as `asm816 <stem>.s` in a scratch directory and
# must write the flat binary to `<stem>` there, matching merlin32's output name.
set -eu

asm816=$1
merlin=$2
lib=$3
src=$4

stem=$(basename "$src" .s)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cp "$src" "$work/$stem.s"
cp -r "$(dirname "$0")/fixtures/." "$work/"

(cd "$work" && "$merlin" -v "$lib" "$stem.s" >/dev/null)
mv "$work/$stem" "$work/$stem.oracle"

(cd "$work" && "$asm816" -m "$lib" "$stem.s")

cmp "$work/$stem.oracle" "$work/$stem"
