#!/bin/sh
# Show merlin32's listing and object bytes for one source file.
#
# usage: probe_merlin.sh <case.s> [listing-lines]
# env:   MERLIN32 (default /root/merlin32/Source/merlin32)
#        MERLIN_LIB (default /root/merlin32/Library)
set -eu

merlin=${MERLIN32:-/root/merlin32/Source/merlin32}
lib=${MERLIN_LIB:-/root/merlin32/Library}
src=$1
lines=${2:-40}

stem=$(basename "$src" .s)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cp "$src" "$work/$stem.s"
(cd "$work" && "$merlin" -v "$lib" "$stem.s" >/dev/null 2>&1) || true

out=$(sed 's/=.*//' "$work/_FileInformation.txt" 2>/dev/null | head -1)
out=${out:-$stem}

if [ -f "$work/${out}_Output.txt" ]; then
  sed -n '/^ Line/,$p' "$work/${out}_Output.txt" | sed 's/\xc2\xab/[<<]/g' | cut -c1-120 | grep -v '^-----' | head -"$lines"
fi
if [ -f "$work/$out" ]; then
  echo "bytes: $(od -An -tx1 -v "$work/$out" | tr -s ' \n' ' ')"
else
  echo "no binary produced"
fi
