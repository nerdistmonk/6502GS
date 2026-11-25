#!/bin/sh
# Build asm816 with AddressSanitizer and UBSan, then run ctest.
# Includes the merlin32 oracle tests when merlin32 is present.
#
# usage: build_asan.sh [build-dir]   (default /tmp/build-asan)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$here/..
build=${1:-/tmp/build-asan}
merlin=${MERLIN32:-/root/merlin32/Source/merlin32}
lib=${MERLIN_LIB:-/root/merlin32/Library}

flags="-fsanitize=address,undefined -fno-omit-frame-pointer"
if [ -x "$merlin" ]; then
  cmake -S "$repo" -B "$build" -DCMAKE_C_FLAGS="$flags" \
    -DMERLIN32_BIN="$merlin" -DMERLIN32_LIB="$lib" >/dev/null
else
  cmake -S "$repo" -B "$build" -DCMAKE_C_FLAGS="$flags" >/dev/null
fi
cmake --build "$build" -j"$(nproc)"
cd "$build" && ctest --output-on-failure
