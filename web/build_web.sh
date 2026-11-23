#!/bin/bash
set -e
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WEB_DIR="$REPO_DIR/web"
OUT_DIR="$REPO_DIR/docs"
LVGL_DIR="$WEB_DIR/lvgl"

mkdir -p "$OUT_DIR"

if [ ! -d "$LVGL_DIR/src" ]; then
  git clone --depth 1 --branch v8.3.11 https://github.com/lvgl/lvgl.git "$LVGL_DIR"
fi

# Collect LVGL 8.3 core, draw, font, hal, misc, and widget C sources
LVGL_SRCS=$(find \
  "$LVGL_DIR/src/core" \
  "$LVGL_DIR/src/draw" \
  "$LVGL_DIR/src/font" \
  "$LVGL_DIR/src/hal" \
  "$LVGL_DIR/src/misc" \
  "$LVGL_DIR/src/widgets" \
  "$LVGL_DIR/src/extra/lv_extra.c" \
  -name '*.c' | sort)

echo "Compiling LVGL 8.3 + ipod/ipod_ui.c to WebAssembly..."
emcc -O2 \
  -I"$WEB_DIR" \
  -I"$LVGL_DIR" \
  -I"$REPO_DIR/ipod" \
  -DLV_CONF_INCLUDE_SIMPLE \
  -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare \
  "$WEB_DIR/main.c" \
  "$REPO_DIR/ipod/ipod_ui.c" \
  $LVGL_SRCS \
  -s EXPORTED_FUNCTIONS='["_main","_web_pointer_event","_web_get_fb","_web_nav_page"]' \
  -s EXPORTED_RUNTIME_METHODS='["HEAPU8"]' \
  -s ALLOW_MEMORY_GROWTH=0 \
  -s INITIAL_MEMORY=16777216 \
  -o "$OUT_DIR/ipod_wasm.js"

ls -lh "$OUT_DIR/ipod_wasm.js" "$OUT_DIR/ipod_wasm.wasm"
echo "Built interactive WASM demo in $OUT_DIR/"
