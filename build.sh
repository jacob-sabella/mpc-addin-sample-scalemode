#!/usr/bin/env bash
# Build the addin for AArch64 (MPC Gen2 / Sample). Needs Docker with QEMU for arm64v8.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
docker run --rm --platform linux/arm64 -u "$(id -u):$(id -g)" -v "$PWD":/b -w /b arm64v8/gcc:12-bookworm sh -c '
  set -e
  F="-std=gnu11 -O2 -Wall -Wextra -Werror"
  gcc $F -fPIC -shared -fvisibility=hidden -Wl,-z,defs -Wl,--as-needed \
    -o build/mpc_sample_scalemode.so src/sample_scalemode.c -lpthread -ldl -lm
  strip --strip-unneeded build/mpc_sample_scalemode.so
  readelf -d build/mpc_sample_scalemode.so | grep NEEDED
  gcc $F -static -o build/padctl src/padctl.c
  strip build/padctl
'
cp -f mpc_sample_scalemode.conf addin.manifest build/
ls -l build/mpc_sample_scalemode.so build/padctl
