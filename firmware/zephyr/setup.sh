#!/bin/bash
# Makes the Zephyr workspace the nRF52840 build needs, once: the Zephyr SDK's ARM toolchain and a
# Zephyr checkout carrying only the modules an nRF52840 Bluetooth build uses (about 3 GB, most of
# it Zephyr's own history). Then `make -C firmware nrf` builds.
#
#   firmware/zephyr/setup.sh [workspace]      default: zephyr-ws beside this repository
#
# Pinned like the ESP32 platform, and for the same reason: an unpinned Zephyr moves under the
# build. v4.5.0-rc1 is the first tag with the Wio Tracker L1 board; move to v4.5.0 once released.
set -euo pipefail

ZEPHYR_REV=v4.5.0-rc1
SDK_VER=1.0.1
MODULES='-.*,+cmsis,+cmsis_6,+hal_nordic,+mbedtls,+tf-psa-crypto,+picolibc'

repo=$(cd "$(dirname "$0")/../.." && pwd)
ws=$(realpath -m "${1:-$repo/../zephyr-ws}")
mkdir -p "$ws"
cd "$ws"

[ -d .venv ] || python3 -m venv .venv
. .venv/bin/activate
pip install -q --upgrade pip west cmake

sdk=zephyr-sdk-$SDK_VER
if [ ! -d "$sdk/gnu/arm-zephyr-eabi" ]; then
    base=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v$SDK_VER
    mkdir -p dl
    for f in "${sdk}_linux-x86_64_minimal.tar.xz" toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz \
             sha256.sum; do
        [ -f "dl/$f" ] || curl -sSfL -o "dl/$f" "$base/$f"
    done
    (cd dl && grep -E "${sdk}_linux-x86_64_minimal.tar.xz|toolchain_gnu_linux-x86_64_arm-zephyr-eabi" \
        sha256.sum | sha256sum -c -)
    tar xf "dl/${sdk}_linux-x86_64_minimal.tar.xz"
    mkdir -p "$sdk/gnu"
    tar xf dl/toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz -C "$sdk/gnu"
fi

[ -d .west ] || west init -m https://github.com/zephyrproject-rtos/zephyr --mr "$ZEPHYR_REV" .
# west init pins Zephyr only when it clones it, and west update never moves Zephyr itself, so a
# workspace made for another revision is caught here rather than built from.
git -C zephyr fetch -q origin "refs/tags/$ZEPHYR_REV:refs/tags/$ZEPHYR_REV" 2>/dev/null || true
want=$(git -C zephyr rev-parse -q --verify "$ZEPHYR_REV^{commit}" || true)
if [ -z "$want" ] || [ "$want" != "$(git -C zephyr rev-parse HEAD)" ]; then
    echo "$ws/zephyr is at $(git -C zephyr describe --tags --always), not $ZEPHYR_REV." >&2
    echo "Move it with: git -C $ws/zephyr checkout $ZEPHYR_REV - then run this again." >&2
    exit 1
fi
west config manifest.project-filter -- "$MODULES"
west update --narrow -o=--depth=1
pip install -q -r zephyr/scripts/requirements-base.txt

# What a build sources: the venv, the SDK found without registering it under ~/.cmake, and the
# same check again, so a Zephyr moved by hand since is refused at build time too.
cat > env.sh <<ENV
. "$ws/.venv/bin/activate"
export ZEPHYR_SDK_INSTALL_DIR="$ws/$sdk"
if [ "\$(git -C "$ws/zephyr" rev-parse HEAD)" != "$want" ]; then
    echo "$ws/zephyr is no longer at $ZEPHYR_REV: run firmware/zephyr/setup.sh" >&2
    return 1
fi
ENV
echo "Zephyr $ZEPHYR_REV and SDK $SDK_VER ready in $ws"
