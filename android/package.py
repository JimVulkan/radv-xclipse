#!/usr/bin/env python3
"""Package a stripped driver as an adrenotools-style zip: meta.json, vulkan.radeon.so and the
third-party notices (NOTICE.txt).

usage: package.py <stripped vulkan.radeon.so> <output dir>
"""
import json
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git(*args):
    try:
        run = lambda *a: subprocess.run(['git', '-C', ROOT, *a], capture_output=True, text=True,
                                        check=True).stdout.strip()
        # Only this repository's history, never an enclosing one.
        if os.path.normcase(os.path.abspath(run('rev-parse', '--show-toplevel'))) != os.path.normcase(ROOT):
            return ''
        return run(*args)
    except (OSError, subprocess.CalledProcessError):
        return ''


def unstripped_sections(path):
    """Names of symbol-table and debug sections left in an ELF64 little-endian file."""
    data = open(path, 'rb').read()
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        sys.exit('error: %s is not a 64-bit little-endian ELF file' % path)
    shoff = int.from_bytes(data[0x28:0x30], 'little')
    shentsize = int.from_bytes(data[0x3a:0x3c], 'little')
    shnum = int.from_bytes(data[0x3c:0x3e], 'little')
    shstrndx = int.from_bytes(data[0x3e:0x40], 'little')
    sections = [data[shoff + i * shentsize:shoff + (i + 1) * shentsize] for i in range(shnum)]
    strtab = sections[shstrndx]
    str_off = int.from_bytes(strtab[0x18:0x20], 'little')
    names = []
    for s in sections:
        name_off = str_off + int.from_bytes(s[0:4], 'little')
        names.append(data[name_off:data.index(b'\0', name_off)].decode('ascii', 'replace'))
    return [n for n in names if n == '.symtab' or n.startswith('.debug')]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    lib, out_dir = sys.argv[1], sys.argv[2]

    leftovers = unstripped_sections(lib)
    if leftovers:
        sys.exit('error: %s is not stripped (%s); run llvm-strip first' % (lib, ', '.join(leftovers[:4])))

    mesa_version = open(os.path.join(ROOT, 'VERSION')).read().strip()
    header = open(os.path.join(ROOT, 'include', 'vulkan', 'vulkan_core.h')).read()
    api = re.search(r'VK_HEADER_VERSION_COMPLETE VK_MAKE_API_VERSION\(0, (\d+), (\d+), VK_HEADER_VERSION\)', header)
    patch = re.search(r'#define VK_HEADER_VERSION (\d+)', header)
    vulkan = 'Vulkan %s.%s.%s' % (api.group(1), api.group(2), patch.group(1))

    commit = git('rev-parse', '--short', 'HEAD')
    count = git('rev-list', '--count', 'HEAD') or '1'
    suffix = '-' + commit if commit else ''

    meta = {
        'schemaVersion': 1,
        'name': 'RADV Xclipse (Mesa %s%s)' % (mesa_version, suffix),
        'description': 'RADV for Samsung Xclipse GPUs, based on Mesa %s.' % mesa_version,
        'author': 'radv-xclipse',
        'packageVersion': count,
        'vendor': 'Mesa',
        'driverVersion': vulkan,
        'minApi': 34,
        'libraryName': 'vulkan.radeon.so',
    }

    os.makedirs(out_dir, exist_ok=True)
    zip_path = os.path.join(out_dir, 'radv-xclipse-%s%s.zip' % (mesa_version, suffix))
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('meta.json', json.dumps(meta, indent=2) + '\n')
        z.write(lib, 'vulkan.radeon.so')
        z.write(os.path.join(ROOT, 'android', 'NOTICE.txt'), 'NOTICE.txt')
    print('packaged %s (%s, %.1f MB)' % (zip_path, vulkan, os.path.getsize(zip_path) / 1e6))


if __name__ == '__main__':
    main()
