#!/usr/bin/env bash
# Creates the Ghidra project ghidra/project/sl6801.gpr with two programs from the flash dump:
# sl6801_app (main firmware) and sl6801_boot (bootloader). Re-running replaces both programs.
# The boot ROM dump is mapped at 0 in both; without it the ROM is a block of bx lr stubs.
# Usage: ghidra/import.sh [path/to/firmware_2M.bin [path/to/bootrom.bin]]
# (GHIDRA_HOME defaults to C:/Tools/ghidra)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
fw="${1:-$here/../firmware_2M.bin}"
rom="${2:-$here/../bootrom.bin}"
ghidra="${GHIDRA_HOME:-C:/Tools/ghidra}"

mkdir -p "$here/project" "$here/input" "$here/out"
cp "$fw" "$here/input/sl6801_app.bin"
cp "$fw" "$here/input/sl6801_boot.bin"
rom_arg=none
if [ -f "$rom" ]; then
	cp "$rom" "$here/input/bootrom.bin"
	rom_arg="$here/input/bootrom.bin"
else
	echo "no boot ROM dump at $rom; using stubs"
fi

for prog in sl6801_boot sl6801_app; do
	"$ghidra/support/analyzeHeadless.bat" "$here/project" sl6801 \
		-import "$here/input/$prog.bin" -overwrite \
		-loader BinaryLoader -loader-baseAddr 0xC00000 \
		-processor ARM:LE:32:Cortex -cspec default \
		-scriptPath "$here/scripts" \
		-preScript SL6801Setup.java "$rom_arg" \
		-postScript SL6801Fixups.java \
		-postScript SL6801Names.java "$here/out" \
		-log "$here/out/$prog.log" -scriptlog "$here/out/${prog}_script.log"
done
