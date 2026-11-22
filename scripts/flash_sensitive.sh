#!/bin/bash
set -e
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
"$REPO_DIR/smartlink_flash/smtlink_dump" init \
  erase_flash 0xd000 0x1000 write_flash 0xd000 0xd000 0x1000 "$REPO_DIR/firmware/sl6801_sensitive.bin" \
  erase_flash 0x1ad000 0x1000 write_flash 0x1ad000 0x1ad000 0x1000 "$REPO_DIR/firmware/sl6801_sensitive.bin" \
  erase_flash 0x1bd000 0x1000 write_flash 0x1bd000 0x1bd000 0x1000 "$REPO_DIR/firmware/sl6801_sensitive.bin"
echo "Done! Unplug USB and long-press Power to boot."
