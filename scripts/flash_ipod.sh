#!/bin/bash
set -e
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
"$REPO_DIR/smartlink_flash/smtlink_dump" init \
  erase_flash 0xd000 0x1000 write_flash 0xd000 0xd000 0x1000 "$REPO_DIR/firmware/sl6801_ipod.bin" \
  erase_flash 0x19000 0x9000 write_flash 0x19000 0x19000 0x9000 "$REPO_DIR/firmware/sl6801_ipod.bin" \
  erase_flash 0x17d000 0x1000 write_flash 0x17d000 0x17d000 0x1000 "$REPO_DIR/firmware/sl6801_ipod.bin" \
  erase_flash 0x1ad000 0x1000 write_flash 0x1ad000 0x1ad000 0x1000 "$REPO_DIR/firmware/sl6801_ipod.bin" \
  erase_flash 0x1bd000 0x1000 write_flash 0x1bd000 0x1bd000 0x1000 "$REPO_DIR/firmware/sl6801_ipod.bin"
echo "Flashed iPod Classic firmware! Unplug USB and long-press Power to boot."
