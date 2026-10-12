#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
VERSION="$(tr -d ' \r\n' < "$ROOT/VERSION")"

if [ -z "${SDK_ROOT:-}" ]; then
  if [ -x "$HOME/tb/sdk/host/bin/aarch64-none-linux-gnu-g++" ]; then
    SDK_ROOT="$HOME/tb/sdk"
  else
    SDK_ROOT="$ROOT/../sdk-tg5050/sdk_tg5050_linux_v1.0.0"
  fi
fi

CXX="$SDK_ROOT/host/bin/aarch64-none-linux-gnu-g++"
STRIP="$SDK_ROOT/host/bin/aarch64-none-linux-gnu-strip"
SYSROOT="$SDK_ROOT/host/aarch64-buildroot-linux-gnu/sysroot"

if [ ! -x "$CXX" ]; then
  echo "Khong tim thay TG5050 SDK compiler: $CXX" >&2
  exit 1
fi

make -C "$ROOT" clean build \
  CXX="$CXX" STRIP="$STRIP" SYSROOT="$SYSROOT" VERSION="$VERSION" \
  -j"${JOBS:-4}"
"$STRIP" --strip-unneeded "$ROOT/files/bin/trimui-localsend"
chmod +x "$ROOT/files/bin/trimui-localsend"
file "$ROOT/files/bin/trimui-localsend" 2>/dev/null || true
