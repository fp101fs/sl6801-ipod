#!/usr/bin/env bash
# Decompile functions from the existing project without re-analyzing.
# Usage: ghidra/decompile.sh <app|boot> <output.c> <function name or 0xaddr>...
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
ghidra="${GHIDRA_HOME:-C:/Tools/ghidra}"
prog="sl6801_$1.bin"
out="$2"
shift 2
"$ghidra/support/analyzeHeadless.bat" "$here/project" sl6801 -process "$prog" -noanalysis -readOnly \
	-scriptPath "$here/scripts" -postScript Decompile.java "$out" "$@" > "$here/out/decompile_console.log" 2>&1
echo "wrote $out"
