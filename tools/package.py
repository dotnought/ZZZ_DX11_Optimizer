#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Package the capture DLL, public PresentMon CLI, and rebuild instructions."""

import argparse
import hashlib
from pathlib import Path
import struct
from zipfile import ZIP_DEFLATED, ZipFile

PRESENTMON_SHA = "b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def validate_dll(data):
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Expected a Windows PE file")
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[offset:offset + 6] != b"PE\0\0\x64\x86" or len(data) < offset + 24:
        raise ValueError("Expected an x64 PE file")
    characteristics = struct.unpack_from("<H", data, offset + 22)[0]
    if not characteristics & 0x2000:
        raise ValueError("Expected a Windows DLL")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--presentmon", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    dll, pm = args.dll.read_bytes(), args.presentmon.read_bytes()
    validate_dll(dll)
    if sha256(pm) != PRESENTMON_SHA:
        parser.error("PresentMon does not match the pinned release executable")
    root = Path(__file__).resolve().parents[1]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(args.output, "w", ZIP_DEFLATED, compresslevel=9) as archive:
        paths = [root / name for name in ("README.md", "LICENSE.GPL.txt")]
        for folder in ("config", "docs", "native", "patches", "tests", "tools", "third-party"):
            paths.extend(path for path in (root / folder).rglob("*")
                         if path.is_file() and path.suffix not in (".pyc", ".exe"))
        for path in sorted(paths):
            archive.write(path, path.relative_to(root).as_posix())
        archive.writestr("bin/d3d11.dll", dll)
        archive.writestr("bin/d3d11.sha256", sha256(dll) + "\n")
        archive.writestr("third-party/PresentMon/PresentMon-2.6.0-x64.exe", pm)
    with ZipFile(args.output) as archive:
        if archive.testzip():
            raise RuntimeError("A packaged file failed its checksum")
    print(f"{sha256(args.output.read_bytes())}  {args.output.name}")


if __name__ == "__main__":
    main()
