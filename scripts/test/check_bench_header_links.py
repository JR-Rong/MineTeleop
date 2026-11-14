#!/usr/bin/env python3
"""Static contract for the bench-only headers (cpp/bench/include/mine_teleop/bench).

1. Every target whose sources include a bench header must link mine_teleop_bench. The check is static on
   purpose: the Windows- and macOS-only targets that use these headers are never compiled by the Linux build.
2. The runtime library (cpp/src and the runtime include tree) must never include a bench header, so the
   bench headers cannot leak back into what the runtime installs.
"""
import pathlib
import re
import sys

repo = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]).resolve()
cmake = (repo / "CMakeLists.txt").read_text()
sources_to_targets = {}
for match in re.finditer(r"add_(?:executable|library)\(\s*([\w\-]+)\s+(?:WIN32\s+|MODULE\s+)?([^)]*)\)", cmake):
    for token in match.group(2).split():
        if token.endswith((".cpp", ".mm")):
            sources_to_targets.setdefault(token, []).append(match.group(1))
linked = set(re.findall(r"target_link_libraries\(\s*([\w\-]+)\s[^)]*\bmine_teleop_bench\b", cmake))

problems, pairs = [], 0
for source in sorted((repo / "cpp").rglob("*")):
    if source.suffix not in {".cpp", ".mm"} or not source.is_file():
        continue
    if "mine_teleop/bench/" not in source.read_text(errors="replace"):
        continue
    relative = str(source.relative_to(repo))
    owners = sources_to_targets.get(relative)
    if not owners:
        problems.append(f"{relative}: includes bench headers but no CMake target compiles it")
        continue
    for owner in owners:
        pairs += 1
        if owner not in linked:
            problems.append(f"{relative}: target {owner} does not link mine_teleop_bench")

runtime_files = list((repo / "cpp/src").rglob("*.cpp")) + list((repo / "cpp/include").rglob("*.hpp"))
for path in runtime_files:
    if "mine_teleop/bench/" in path.read_text(errors="replace"):
        problems.append(f"{path.relative_to(repo)}: the runtime must not include bench headers")

if "add_library(mine_teleop_bench INTERFACE)" not in cmake:
    problems.append("CMakeLists.txt does not define mine_teleop_bench")
print(f"bench header contract: {pairs} source/target pairs, {len(runtime_files)} runtime files, {len(problems)} problems")
for problem in problems:
    print(problem)
sys.exit(1 if problems else 0)
