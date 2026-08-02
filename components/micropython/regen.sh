#!/bin/sh
# Regenerate the MicroPython embed package.
#
# micropython_embed/ is generated output and is not checked in. Run this after
# changing mpconfigport.h — the qstr tables and module registry are baked in at
# generation time, so a config change alone will not take effect.
#
# Usage:  MICROPYTHON_TOP=/path/to/micropython ./regen.sh
set -e

MICROPYTHON_TOP="${MICROPYTHON_TOP:-$(cd "$(dirname "$0")/../../.." && pwd)/micropython}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ ! -f "$MICROPYTHON_TOP/ports/embed/embed.mk" ]; then
    echo "No micropython checkout at $MICROPYTHON_TOP" >&2
    echo "  git clone --depth 1 https://github.com/micropython/micropython.git" >&2
    exit 1
fi

cd "$HERE"
rm -rf micropython_embed build-embed
# embed_extra.mk, not embed.mk directly: it widens the qstr/module scan to the
# extmod sources we copy in below.
make -f embed_extra.mk \
     MICROPYTHON_TOP="$MICROPYTHON_TOP" \
     BUILD=build-embed EMBED_DIR=micropython_embed

# The embed port ships extmod/modplatform.h and no extmod C sources at all, so
# any extmod module we enable in mpconfigport.h has to be copied in by hand.
# Without this, MICROPY_PY_TIME=1 builds and links cleanly and then fails at
# runtime with "ImportError: no module named 'time'".
for f in modtime.c modtime.h; do
    cp "$MICROPYTHON_TOP/extmod/$f" micropython_embed/extmod/
    echo "+ extmod/$f"
done

# The component globs its sources at configure time, so a newly generated file
# is invisible until cmake re-runs. Touching the CMakeLists is what makes ninja
# regenerate; without it the next build links against a stale file list.
touch CMakeLists.txt

echo "micropython_embed regenerated"
