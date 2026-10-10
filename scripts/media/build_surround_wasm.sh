#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
output_dir="${1:-$repo_dir/cpp/web/assets}"
mkdir -p "$output_dir"
emcc "$repo_dir/cpp/src/surround_math.cpp" -I "$repo_dir/cpp/include" -std=c++20 -O3 \
  -sMODULARIZE=1 -sEXPORT_NAME=createSurroundModule -sALLOW_MEMORY_GROWTH=1 \
  -sENVIRONMENT=worker -sFILESYSTEM=0 \
  '-sEXPORTED_FUNCTIONS=["_malloc","_free","_mt_surround_create","_mt_surround_render","_mt_surround_destroy","_mt_surround_effective_mask"]' \
  -o "$output_dir/surround_core.js"
python3 - "$output_dir/surround_core.js" <<'PY'
from pathlib import Path
import sys
path=Path(sys.argv[1])
path.write_text('\n'.join(line.rstrip() for line in path.read_text().splitlines())+'\n')
PY
chmod 0644 "$output_dir/surround_core.wasm"
cp "$repo_dir/cpp/web/surround_worker.js" "$output_dir/surround_worker.js"
