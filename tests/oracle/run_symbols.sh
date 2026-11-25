#!/bin/sh
# Black-box oracle check for the symbol table: assemble one source with
# merlin32 and asm816 at each column count and require identical tables.
#
# usage: run_symbols.sh <asm816> <merlin32> <merlin-library-dir> <case.s>
#
# Contract: merlin32 and asm816 write their tables to <stem>_Output.txt when
# given -v<N> or --verbose, and print them to stdout when given -s with -v.
# Only the text from the "Symbol Table - Numerical Order" line to the end of
# the output is compared.
set -eu

asm816=$1
merlin=$2
lib=$3
src=$4

stem=$(basename "$src" .s)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cp "$src" "$work/$stem.s"

for cols in 0 1 2 3 9 10; do
    (cd "$work" && "$merlin" -v$cols "$lib" "$stem.s" >/dev/null </dev/null)
    sed -n '/^Symbol Table - Numerical Order$/,$p' "$work/${stem}_Output.txt" >"$work/oracle.$cols"
    (cd "$work" && "$asm816" -v$cols -m "$lib" "$stem.s")
    sed -n '/^Symbol Table - Numerical Order$/,$p' "$work/${stem}_Output.txt" >"$work/ours.$cols"
    cmp "$work/oracle.$cols" "$work/ours.$cols"
done

# Bare --verbose writes the tables like -v0, and -s prints them to stdout after -v.
(cd "$work" && "$merlin" --verbose "$lib" "$stem.s" >/dev/null </dev/null)
sed -n '/^Symbol Table - Numerical Order$/,$p' "$work/${stem}_Output.txt" >"$work/oracle.long"
(cd "$work" && "$asm816" --verbose -m "$lib" "$stem.s")
sed -n '/^Symbol Table - Numerical Order$/,$p' "$work/${stem}_Output.txt" >"$work/ours.long"
cmp "$work/oracle.long" "$work/ours.long"

(cd "$work" && "$merlin" -v3 -s "$lib" "$stem.s" </dev/null) |
    sed -n '/^Symbol Table - Numerical Order$/,$p' >"$work/oracle.console"
(cd "$work" && "$asm816" -v3 -s -m "$lib" "$stem.s") |
    sed -n '/^Symbol Table - Numerical Order$/,$p' >"$work/ours.console"
cmp "$work/oracle.console" "$work/ours.console"
