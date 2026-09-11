#!/usr/bin/env bash
#
# examples/hello_text/apply.sh
#
# Back up rp/ to rp.bak and assets/ to assets.bak, then customize rp/ to
# build the hello_text example: remove the bundled demos and the
# template's asset sources, and install this example's emul.c +
# CMakeLists.txt. Run it from anywhere in a template checkout.
#
# Revert with:   rm -rf rp assets && mv rp.bak rp && mv assets.bak assets
#
set -euo pipefail

# Resolve the repo root from this script's own location (examples/hello_text).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

if [ ! -d rp ]; then
  echo "error: no rp/ directory at $ROOT -- run from a template checkout." >&2
  exit 1
fi
if [ -e rp.bak ] || [ -e assets.bak ]; then
  echo "error: rp.bak / assets.bak already exists -- refusing to overwrite" >&2
  echo "       the backup. Restore or remove it first (see 'Revert' below)." >&2
  exit 1
fi

echo "Backing up rp/ -> rp.bak ..."
cp -a rp rp.bak
REVERT="rm -rf rp && mv rp.bak rp"
if [ -d assets ]; then
  echo "Backing up assets/ -> assets.bak ..."
  cp -a assets assets.bak
  REVERT="rm -rf rp assets && mv rp.bak rp && mv assets.bak assets"
fi

echo "Removing demo sources + asset headers ..."
rm -f rp/src/demo_menu.c rp/src/demo_parallax.c rp/src/demo_3d.c \
      rp/src/demo_sprites.c rp/src/demo_cojorotozoom.c
rm -f rp/src/include/demo.h \
      rp/src/include/sidecart_logo.h rp/src/include/sidecart_text.h \
      rp/src/include/solid3d.h rp/src/include/sprites_data.h \
      rp/src/include/cojo_texture.h rp/src/include/cojo_font.h \
      rp/src/include/diego_sprite.h rp/src/include/uridium_surface.h

# assets/ holds the *sources* the bundled headers were generated from
# (e.g. demo_jingle.sam -> rp/src/include/audio_sample.h). A new app
# brings its own, and the generated headers it still uses are already in
# rp/src/include, so the sources go with the demos.
echo "Removing the template's asset sources ..."
rm -rf assets

echo "Installing the hello_text app (emul.c + CMakeLists.txt) ..."
cp "$SCRIPT_DIR/emul.c" rp/src/emul.c
cp "$SCRIPT_DIR/CMakeLists.txt" rp/src/CMakeLists.txt

cat <<EOF

Done. rp/ now builds the hello_text example; the originals are backed up.

  Build:   ./build.sh pico_w release 44444444-4444-4444-8444-444444444444
  Revert:  $REVERT
EOF
