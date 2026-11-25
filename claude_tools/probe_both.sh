#!/bin/sh
# Assemble one source with merlin32 and asm816 side by side, without asserting.
# Prints each tool's bytes and stderr, then a diff of the listings.
# For pass/fail checks use tests/oracle/run_oracle.sh instead.
#
# usage: probe_both.sh <case.s>
# env:   MERLIN32 (default /root/merlin32/Source/merlin32)
#        MERLIN_LIB (default /root/merlin32/Library)
#        ASM816 (default <repo>/build/asm816)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
merlin=${MERLIN32:-/root/merlin32/Source/merlin32}
lib=${MERLIN_LIB:-/root/merlin32/Library}
asm816=${ASM816:-$here/../build/asm816}
src=$1

stem=$(basename "$src" .s)
m=$(mktemp -d)
o=$(mktemp -d)
trap 'rm -rf "$m" "$o"' EXIT

cp "$src" "$m/$stem.s"
cp "$src" "$o/$stem.s"
cp -r "$here/../tests/oracle/fixtures/." "$m/"
cp -r "$here/../tests/oracle/fixtures/." "$o/"

(cd "$m" && "$merlin" -v "$lib" "$stem.s" >"$m/stdout.txt" 2>&1) || true
(cd "$o" && "$asm816" -v -m "$lib" "$stem.s" >"$o/stdout.txt" 2>&1) || true

name() {
  out=$(sed 's/=.*//' "$1/_FileInformation.txt" 2>/dev/null | head -1)
  echo "${out:-$stem}"
}
mout=$(name "$m")
oout=$(name "$o")

echo "== merlin32 output"
[ -f "$m/$mout" ] && echo "bytes: $(od -An -tx1 -v "$m/$mout" | tr -s ' \n' ' ')" || echo "no binary"
grep -i 'error' "$m/stdout.txt" | head -3 || true
echo "== asm816 output"
[ -f "$o/$oout" ] && echo "bytes: $(od -An -tx1 -v "$o/$oout" | tr -s ' \n' ' ')" || echo "no binary"
grep -i 'error\|warning' "$o/stdout.txt" | head -3 || true

if [ -f "$m/${mout}_Output.txt" ] && [ -f "$o/${oout}_Output.txt" ]; then
  echo "== listing diff (< merlin32, > asm816)"
  diff "$m/${mout}_Output.txt" "$o/${oout}_Output.txt" || true
fi
