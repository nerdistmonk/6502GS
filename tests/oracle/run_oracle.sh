#!/bin/sh
# Black-box oracle check: assemble one source with merlin32 and asm816 and
# require identical flat binaries and _FileInformation.txt entries.
#
# usage: run_oracle.sh <asm816> <merlin32> <merlin-library-dir> <case.s>
#
# Contract: asm816 is invoked as `asm816 -m <lib> <stem>.s` in a scratch
# directory. Like merlin32, it names the binary after the first SAV or DSK
# (else <stem>) and writes _FileInformation.txt beside it.
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
out=$(sed 's/=.*//' "$work/_FileInformation.txt")
mv "$work/$out" "$work/$out.oracle"
mv "$work/_FileInformation.txt" "$work/_FileInformation.oracle"

(cd "$work" && "$asm816" -m "$lib" "$stem.s")

cmp "$work/$out.oracle" "$work/$out"
cmp "$work/_FileInformation.oracle" "$work/_FileInformation.txt"
