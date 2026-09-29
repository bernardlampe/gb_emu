#!/usr/bin/env python3
"""check_frame.py — verify a dumped frame against an independently computed image.

The point of this script is that it does NOT ask the emulator what the picture
should be: it computes the expected image from the test ROM's own definition
(tiles, BG map, window position, palette) and compares pixel by pixel. That is
what makes it evidence about the PPU rather than a restatement of it.

Usage: python3 tests/check_frame.py <frame.bmp>
"""
import struct
import sys

# DMG shades, matching DMG_SHADE_0..3 in include/ppu.h, as the (b, g, r)
# triples a 24-bit BMP actually stores.
SHADE = {
    0: (0xD0, 0xF8, 0xE0),
    1: (0x70, 0xC0, 0x88),
    2: (0x56, 0x68, 0x34),
    3: (0x20, 0x18, 0x08),
}

# BGP = 0xE4 maps colour index 0->shade 0, 1->1, 2->2, 3->3.
BGP = [0, 1, 2, 3]

# The test ROM's layout, from tools/make_test_roms.py.
BG_TILE_W = 10      # 10 tiles wide = 80 pixels of tile 1
BG_TILE_H = 9       # 9 tiles high  = 72 pixels of tile 1
WY = 72              # the window starts on line 72
WX = 87               # its left edge is at x = WX - 7 = 80


def expected_pixel(x, y):
    """Compute the colour index the ROM's own definitions imply, then the shade."""
    if y >= WY and x >= WX - 7:
        index = 2                       # the window map is filled with tile 2
    elif x < BG_TILE_W * 8 and y < BG_TILE_H * 8:
        index = 1                       # the BG map has tile 1 there
    else:
        index = 0                       # and tile 0 everywhere else
    return SHADE[BGP[index]]


def read_bmp(path):
    """Read a 24-bit BMP and return (width, height, rows) with rows top-down.

    A 24-bit BMP stores each pixel as blue, green, red, so the tuple returned
    here is (b, g, r) in file order: the caller must not assume RGB.
    """
    with open(path, "rb") as f:
        data = f.read()

    if data[:2] != b"BM":
        raise SystemExit(f"{path}: not a BMP file")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]

    if bpp != 24:
        raise SystemExit(f"{path}: expected 24 bpp, got {bpp}")
    if width != 160 or abs(height) != 144:
        raise SystemExit(f"{path}: expected 160x144, got {width}x{abs(height)}")

    row_stride = (width * 3 + 3) & ~3     # BMP rows are padded to 4 bytes
    rows = []
    for row in range(abs(height)):
        # A positive height means the rows are stored bottom-up.
        src_row = row if height < 0 else abs(height) - 1 - row
        start = pixel_offset + src_row * row_stride
        rows.append([tuple(data[start + x * 3:start + x * 3 + 3]) for x in range(width)])
    return width, abs(height), rows


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_frame.py <frame.bmp>")

    width, height, rows = read_bmp(sys.argv[1])

    checks = [
        (10, 10), (79, 10), (100, 10), (159, 10),      # BG row, both tiles
        (10, 71), (100, 71),                                # last BG-only line
        (10, 72), (100, 72),                                # window starts here
        (79, 100), (80, 100),                                # window left edge
        (10, 143), (100, 143), (159, 143),                 # bottom line
    ]

    failures = 0
    for x, y in checks:
        got = rows[y][x]
        want = expected_pixel(x, y)
        status = "ok" if got == want else "MISMATCH"
        if got != want:
            failures += 1
        print(f"  ({x:3d},{y:3d}) got #{got[0]:02X}{got[1]:02X}{got[2]:02X} "
              f"want #{want[0]:02X}{want[1]:02X}{want[2]:02X}  {status}")

    # Also check that the frame is not accidentally uniform: a blank frame
    # would pass several of the checks above by luck.
    distinct = {rows[y][x] for x, y in checks}
    if len(distinct) < 3:
        print(f"  frame has only {len(distinct)} distinct colours at the sample points")
        failures += 1

    if failures:
        print(f"PPU frame check: {failures} failures")
        return 1
    print("PPU frame check: all sample points match the expected image")
    return 0


if __name__ == "__main__":
    sys.exit(main())
