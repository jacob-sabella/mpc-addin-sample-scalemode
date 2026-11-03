#!/usr/bin/env bash
# Build the addin for AArch64 (MPC Gen2 / Sample). Needs Docker with QEMU for arm64v8.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
docker run --rm --platform linux/arm64 -u "$(id -u):$(id -g)" -v "$PWD":/b -w /b arm64v8/gcc:12-bookworm sh -c '
  set -e
  F="-std=gnu11 -O2 -Wall -Wextra -Werror"
  gcc $F -fPIC -shared -fvisibility=hidden -Wl,-z,defs -Wl,--as-needed \
    -o build/mpc_enable_scalemode.so src/enable_scalemode.c -lpthread
  strip --strip-unneeded build/mpc_enable_scalemode.so
  readelf -d build/mpc_enable_scalemode.so | grep NEEDED
'
cp -f mpc_enable_scalemode.conf addin.manifest build/
ls -l build/mpc_enable_scalemode.so
