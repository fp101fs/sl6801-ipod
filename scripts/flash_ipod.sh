#!/bin/bash
set -e
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
"$REPO_DIR/smartlink_flash/smtlink_dump" init \
  write_flash 0 0 0 "$REPO_DIR/firmware/sl6801_ipod.bin" \
  check_flash 0xe000 0x1ea8f0
echo "Verified & flashed iPod Classic firmware! Unplug USB, hold Power 8s to exit bootloader, then press Power 3s to boot."
