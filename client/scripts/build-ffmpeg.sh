#!/usr/bin/env bash
# Builds a minimal libavcodec/libavutil for the 3DS with only the mpeg2video decoder.
# Run from the devkitPro MSYS2 shell (or any shell with devkitARM installed).
#
# Uses FFmpeg_for_3DS (Core-2-Extreme's fork, used by "Video player for 3DS"), pinned to
# the same commit PotatoStream uses. Unlike PotatoStream, assembly stays enabled: the
# ARMv6 IDCT and motion compensation live in .S files that --disable-asm would drop.
set -euo pipefail

: "${DEVKITARM:=/opt/devkitpro/devkitARM}"
: "${DEVKITPRO:=/opt/devkitpro}"
export PATH="$DEVKITARM/bin:$PATH"

COMMIT=67400703903dde32d901368d53c384ba0e234d8b
CLIENT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$CLIENT_DIR/third_party/FFmpeg_for_3DS"
PREFIX="$CLIENT_DIR/third_party/ffmpeg"

if [ ! -d "$SRC/.git" ]; then
    git init -q "$SRC"
    git -C "$SRC" config core.autocrlf false
    git -C "$SRC" remote add origin https://github.com/Core-2-Extreme/FFmpeg_for_3DS.git
fi
if [ "$(git -C "$SRC" rev-parse -q --verify HEAD || true)" != "$COMMIT" ]; then
    git -C "$SRC" fetch -q --depth 1 origin "$COMMIT"
    git -C "$SRC" checkout -q FETCH_HEAD
fi

cd "$SRC"
./configure \
    --prefix="$PREFIX" \
    --enable-cross-compile \
    --cross-prefix=arm-none-eabi- \
    --arch=arm --cpu=armv6k --target-os=none \
    --extra-cflags="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -ffunction-sections -Wno-incompatible-pointer-types -D__3DS__ -I$DEVKITPRO/libctru/include" \
    --extra-ldflags="-specs=3dsx.specs -march=armv6k -mfloat-abi=hard -mtp=soft -L$DEVKITPRO/libctru/lib" \
    --extra-libs="-lctru -lm" \
    --enable-static --disable-shared \
    --disable-autodetect --disable-everything --disable-programs --disable-doc --disable-debug \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale \
    --disable-swresample --disable-postproc --disable-network \
    --disable-pthreads --disable-w32threads --disable-os2threads \
    --disable-runtime-cpudetect \
    --disable-neon --disable-vfpv3 --disable-armv6t2 \
    --enable-armv5te --enable-armv6 --enable-vfp \
    --enable-decoder=mpeg2video

make -j"$(nproc)"
make install

# Make sure the ARMv6 optimizations were built in.
for sym in ff_simple_idct_armv6 ff_put_pixels16_armv6; do
    if ! arm-none-eabi-nm "$PREFIX/lib/libavcodec.a" 2>/dev/null | grep -q " T $sym"; then
        echo "WARNING: $sym is missing; decoding will be slower" >&2
    fi
done
echo "Done: $PREFIX"
