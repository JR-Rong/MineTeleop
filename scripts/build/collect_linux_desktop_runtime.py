#!/usr/bin/env python3
"""Collect native/Electron shared libraries on the target Linux build host.

Keep only glibc/loader and GPU driver interfaces on the desktop OS. No FUSE,
AppImage launcher, Node installation, or system package installation at runtime.
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
root = pathlib.Path(sys.argv[1]).resolve()
controller = root / 'resources/controller'
os_libraries = re.compile(r'^(ld-linux|lib(c|m|pthread|dl|rt|resolv|util|anl)\.so|lib(GL|EGL|GLES|GLX|OpenGL|vulkan|drm|gbm)[-.])')
seen = set()
def is_elf(p):
    if not p.is_file() or p.is_symlink():
        return False
    with p.open('rb') as stream:
        return stream.read(4) == b'\x7fELF'
queue = [p for p in root.rglob('*') if is_elf(p)]
for binary in queue:
    if binary in seen:
        continue
    seen.add(binary)
    result = subprocess.run(['ldd', str(binary)], capture_output=True, text=True)
    if 'not found' in result.stdout:
        raise SystemExit('Missing build-host dependency for ' + str(binary) + '\n' + result.stdout)
    for name, source in re.findall(r'^\s*(\S+) => (/\S+)', result.stdout, re.M):
        if os_libraries.match(name):
            continue
        destination = root / name
        if not destination.exists():
            shutil.copy2(source, destination)
            queue.append(destination)
        private = controller / 'lib' / name
        private.parent.mkdir(parents=True, exist_ok=True)
        if not private.exists():
            shutil.copy2(source, private)
for binary in [root/'MineTeleop',controller/'bin/mine-teleop-control']:
    relative = '$ORIGIN' if binary.parent == root else '$ORIGIN/../lib'
    subprocess.run(['patchelf','--force-rpath','--set-rpath',relative,str(binary)], check=True)
for binary in seen:
    result = subprocess.run(['ldd',str(binary)],capture_output=True,text=True,env={**os.environ,'LD_LIBRARY_PATH':str(root)})
    if 'not found' in result.stdout:
        raise SystemExit('Unresolved packaged library: ' + str(binary) + '\n' + result.stdout)
    for name, source in re.findall(r'^\s*(\S+) => (/\S+)',result.stdout,re.M):
        if not os_libraries.match(name) and not pathlib.Path(source).is_relative_to(root):
            raise SystemExit('Dependency still resolves outside package: ' + name)
print('linux_runtime_files=' + str(len(seen)))
