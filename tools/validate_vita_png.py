#!/usr/bin/env python3
"""Validate a PNG against PS Vita LiveArea image requirements."""
import os
import struct
import sys


def main():
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: validate_vita_png.py FILE EXPECTED_WIDTH EXPECTED_HEIGHT MAX_BYTES"
        )

    path = sys.argv[1]
    expected_width = int(sys.argv[2])
    expected_height = int(sys.argv[3])
    max_bytes = int(sys.argv[4])

    with open(path, "rb") as stream:
        header = stream.read(29)

    if len(header) < 29 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise SystemExit("Not a valid PNG file: " + path)

    width, height = struct.unpack(">II", header[16:24])
    bit_depth = header[24]
    color_type = header[25]
    interlace_method = header[28]
    file_size = os.path.getsize(path)

    problems = []
    if (width, height) != (expected_width, expected_height):
        problems.append(
            "dimensions are %dx%d, expected %dx%d"
            % (width, height, expected_width, expected_height)
        )
    if bit_depth != 8 or color_type != 3:
        problems.append(
            "PNG must use an 8-bit indexed palette (got bit depth %d, color type %d)"
            % (bit_depth, color_type)
        )
    if interlace_method != 0:
        problems.append("interlaced PNGs are not supported by the Vita LiveArea installer")
    if file_size > max_bytes:
        problems.append(
            "file is %d bytes, exceeding the %d-byte limit" % (file_size, max_bytes)
        )

    if problems:
        raise SystemExit("%s: " % path + "; ".join(problems))

    print(
        "%s: valid %dx%d indexed 8-bit PNG, %d bytes"
        % (path, width, height, file_size)
    )


if __name__ == "__main__":
    main()
