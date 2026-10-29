#!/usr/bin/env python3
"""Validate a user-supplied PNG before embedding it into the Vita LiveArea."""
import struct
import sys

def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: validate_png.py FILE EXPECTED_WIDTH EXPECTED_HEIGHT")
    path = sys.argv[1]
    expected = (int(sys.argv[2]), int(sys.argv[3]))
    with open(path, "rb") as stream:
        header = stream.read(24)
    if len(header) < 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise SystemExit("Not a valid PNG file: " + path)
    actual = struct.unpack(">II", header[16:24])
    if actual != expected:
        raise SystemExit(
            "%s has dimensions %dx%d; expected exactly %dx%d"
            % (path, actual[0], actual[1], expected[0], expected[1])
        )
    print("%s: valid %dx%d PNG" % (path, actual[0], actual[1]))

if __name__ == "__main__":
    main()
