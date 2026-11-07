#!/usr/bin/env bash
# Builds the 3DS client. Run scripts/build-ffmpeg.sh once before this.
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
cd "$(dirname "${BASH_SOURCE[0]}")/.." && make "$@"
