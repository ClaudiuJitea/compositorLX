#!/usr/bin/env python3
"""Copy upstream Camera Raw resources, changing only the table compression for Qt."""
import lzma
import shutil
import struct
import sys
import zlib
from pathlib import Path

source = Path(sys.argv[1]) / "Compositor/Resources"
target = Path(__file__).resolve().parents[1] / "src/resources"
data = (source / "CameraRawTables.bin").read_bytes()
if len(data) < 10 or data[:4] != b"CRT3":
    raise SystemExit("Unsupported upstream Camera Raw table format")
size, count, step = struct.unpack("<HHH", data[4:10])
planes = lzma.decompress(data[10:])
if size != 17 or count != 763 or step == 0 or len(planes) != count * size**3 * 3:
    raise SystemExit("Invalid upstream Camera Raw table dimensions")
target.mkdir(exist_ok=True)
# qUncompress expects the uncompressed byte count in big endian before its zlib stream.
(target / "CameraRawTables.bin").write_bytes(
    b"CRT4" + data[4:10] + struct.pack(">I", len(planes)) + zlib.compress(planes, 9)
)
shutil.copyfile(source / "CameraRawShadows.bin", target / "CameraRawShadows.bin")
