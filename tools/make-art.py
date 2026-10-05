"""Mirage - the title's home screen pictures, in the Dracula colours.
SPDX-License-Identifier: GPL-3.0-or-later

Draws the tile (icon0.png, 512x512) and the two backgrounds (pic0.dds while
the title is selected, pic1.dds while it starts; 3840x2160, BC7) into
title\\sce_sys, and a PNG copy of each background beside them to look at.

    python tools\\make-art.py

Needs numpy and Pillow. The letters are the title's own 5x7 font, read from
title\\src\\demo_renderer.cpp, so the pictures match the idle screen. The BC7
blocks are written here (mode 6 only: two end colours and sixteen steps per
4x4 block), which is exact enough for flat shapes and needs no other tool.
"""
import os
import re
import struct
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "title", "sce_sys")

# Dracula (draculatheme.com), as RGB.
BACKGROUND = (0x28, 0x2A, 0x36)
CURRENT_LINE = (0x44, 0x47, 0x5A)
FOREGROUND = (0xF8, 0xF8, 0xF2)
COMMENT = (0x62, 0x72, 0xA4)
CYAN = (0x8B, 0xE9, 0xFD)
GREEN = (0x50, 0xFA, 0x7B)
ORANGE = (0xFF, 0xB8, 0x6C)
PINK = (0xFF, 0x79, 0xC6)
PURPLE = (0xBD, 0x93, 0xF9)
RED = (0xFF, 0x55, 0x55)
YELLOW = (0xF1, 0xFA, 0x8C)


def read_font():
    source = open(os.path.join(ROOT, "title", "src", "demo_renderer.cpp"), encoding="utf-8").read()
    glyphs = {}
    for character, rows in re.findall(r"\{'(.)',\s*\{([0-9, ]+)\}\}", source):
        glyphs[character] = [int(value) for value in rows.split(",")]
    if len(glyphs) < 40:
        raise SystemExit("could not read the font from demo_renderer.cpp")
    return glyphs


FONT = read_font()


class Picture:
    def __init__(self, width, height):
        self.pixels = np.empty((height, width, 3), np.uint8)
        self.pixels[:, :] = BACKGROUND
        self.width, self.height = width, height

    def rectangle(self, x, y, width, height, color):
        x0, y0 = max(0, int(x)), max(0, int(y))
        self.pixels[y0:max(y0, int(y + height)), x0:max(x0, int(x + width))] = color

    def triangle(self, center_x, top, half_width, height, color, behind, first_row=0, last_row=None):
        """Point up, with smooth sloping sides, as the idle screen's
        smooth_triangle draws it: the pixel at each end of a row is mixed
        with `behind` by how much of it the triangle covers."""
        for row in range(first_row, int(height if last_row is None else last_row)):
            exact = (2 * row + 1) * half_width * 256 // (2 * height)
            whole, part = exact >> 8, exact & 0xFF
            self.rectangle(center_x - whole, top + row, 2 * whole + 1, 1, color)
            if part:
                edge = tuple((color[i] * part + behind[i] * (256 - part) + 128) >> 8 for i in range(3))
                self.rectangle(center_x - whole - 1, top + row, 1, 1, edge)
                self.rectangle(center_x + whole + 1, top + row, 1, 1, edge)

    def text(self, x, y, value, scale, color):
        for character in value:
            for row, bits in enumerate(FONT.get(character, FONT[" "])):
                for column in range(5):
                    if bits & (1 << (4 - column)):
                        self.rectangle(x + column * scale, y + row * scale, scale, scale, color)
            x += 6 * scale

    def centred(self, center_x, y, value, scale, color):
        self.text(center_x - (len(value) * 6 - 1) * scale // 2, y, value, scale, color)

    def mark(self, center_x, top, unit):
        """A screen with a triangle rising into it, as on the idle screen
        (there: 300 wide at unit 1)."""
        u = unit
        self.rectangle(center_x - 150 * u, top, 300 * u, 190 * u, PURPLE)
        self.rectangle(center_x - 136 * u, top + 14 * u, 272 * u, 162 * u, BACKGROUND)
        # The gap around the triangle only shows where it cuts the screen's
        # lower edge: there the purple is behind it.
        self.triangle(center_x, top + 116 * u, 112 * u, 98 * u, BACKGROUND, PURPLE, 60 * u, 74 * u)
        self.triangle(center_x, top + 140 * u, 80 * u, 70 * u, PINK, BACKGROUND)

    def palette(self, center_x, y, size, gap):
        colors = [CYAN, GREEN, ORANGE, PINK, PURPLE, RED, YELLOW]
        x = center_x - (len(colors) * size + (len(colors) - 1) * gap) // 2
        for color in colors:
            self.rectangle(x, y, size, size, color)
            x += size + gap


def tile():
    picture = Picture(512, 512)
    picture.mark(256, 96, 1)
    picture.centred(256, 356, "MIRAGE", 10, FOREGROUND)
    picture.palette(256, 456, 14, 14)
    return picture


def background(center_x):
    """center_x: where the art sits. The home screen puts its own things on
    the left and along the bottom of the selected title's background."""
    picture = Picture(3840, 2160)
    picture.mark(center_x, 520, 2)
    picture.centred(center_x, 1060, "MIRAGE", 28, FOREGROUND)
    picture.centred(center_x, 1320, "AIRPLAY RECEIVER", 8, COMMENT)
    picture.palette(center_x, 1450, 28, 28)
    return picture


# ---- BC7, mode 6 -----------------------------------------------------------

WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.int32)


def bc7_mode6(pixels):
    """pixels: (height, width, 3) uint8, both multiples of 4. Returns the blocks."""
    height, width, _ = pixels.shape
    blocks = pixels.reshape(height // 4, 4, width // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3).astype(np.int32)
    count = blocks.shape[0]
    # The end colours: the darkest and the brightest pixel of the block.
    luma = blocks @ np.array([2, 4, 1], np.int32)
    low = blocks[np.arange(count), luma.argmin(axis=1)]
    high = blocks[np.arange(count), luma.argmax(axis=1)]

    def quantise(color):
        """To 7 bits a channel and one shared low bit; returns (7 bit values, low bit, 8 bit values)."""
        best = None
        for bit in (0, 1):
            seven = np.clip((color - bit + 1) // 2, 0, 127)
            eight = seven * 2 + bit
            error = np.abs(eight - color).sum(axis=1)
            if best is None:
                best = (seven, np.full(count, bit, np.int32), eight, error)
            else:
                better = error < best[3]
                best = (np.where(better[:, None], seven, best[0]), np.where(better, bit, best[1]),
                        np.where(better[:, None], eight, best[2]), np.minimum(error, best[3]))
        return best[0], best[1], best[2]

    low7, low_bit, low8 = quantise(low)
    high7, high_bit, high8 = quantise(high)
    # Each pixel takes the nearest of the sixteen steps between the two ends.
    steps = ((64 - WEIGHTS)[None, :, None] * low8[:, None, :] + WEIGHTS[None, :, None] * high8[:, None, :] + 32) >> 6
    index = np.empty((count, 16), np.int64)
    for start in range(0, count, 65536):  # in pieces: the distances take memory
        part = slice(start, start + 65536)
        distance = ((blocks[part, :, None, :] - steps[part, None, :, :]) ** 2).sum(axis=3)
        index[part] = distance.argmin(axis=2)
    # The first pixel's index is stored without its top bit, which must be 0:
    # where it is not, the ends change places.
    swap = index[:, 0] >= 8
    index = np.where(swap[:, None], 15 - index, index)
    low7, high7 = np.where(swap[:, None], high7, low7), np.where(swap[:, None], low7, high7)
    low_bit, high_bit = np.where(swap, high_bit, low_bit), np.where(swap, low_bit, high_bit)

    # 128 bits, least significant first: mode (7), R0 R1 G0 G1 B0 B1 A0 A1
    # (7 each), P0, P1, then the indices (3 bits, then fifteen of 4).
    lo = np.zeros(count, np.uint64)
    hi = np.zeros(count, np.uint64)
    position = 0

    def put(value, bits):
        nonlocal lo, hi, position
        value = value.astype(np.uint64)
        if position < 64:
            lo |= (value << np.uint64(position)) & np.uint64(0xFFFFFFFFFFFFFFFF)
            if position + bits > 64:
                hi |= value >> np.uint64(64 - position)
        else:
            hi |= value << np.uint64(position - 64)
        position += bits

    put(np.full(count, 1 << 6), 7)
    for channel in range(3):
        put(low7[:, channel], 7)
        put(high7[:, channel], 7)
    # Alpha is opaque at both ends whichever the low bit: (127 << 1) | p is
    # 254 or 255. A background has no use for the difference.
    put(np.full(count, 127), 7)
    put(np.full(count, 127), 7)
    put(low_bit, 1)
    put(high_bit, 1)
    put(index[:, 0], 3)
    for i in range(1, 16):
        put(index[:, i], 4)
    assert position == 128
    return np.stack([lo, hi], axis=1).astype("<u8").tobytes()


def write_dds(path, picture):
    height, width = picture.height, picture.width
    data = bc7_mode6(picture.pixels)
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, 0x000A1007, height, width, len(data), 0, 1)
    header += struct.pack("<II4s20x", 32, 4, b"DX10")          # pixel format: see the DX10 header
    header += struct.pack("<I16x", 0x1000)                      # caps: texture
    header += struct.pack("<IIIII", 98, 3, 0, 1, 0)             # BC7_UNORM, 2D, one picture
    assert len(header) == 148
    with open(path, "wb") as file:
        file.write(header + data)


def main():
    os.makedirs(OUT, exist_ok=True)
    icon = tile()
    Image.fromarray(icon.pixels, "RGB").save(os.path.join(OUT, "icon0.png"))
    print("wrote icon0.png")
    # Selected: the art to the right, clear of the home screen's own text.
    # Starting: in the middle, where the idle screen then has it.
    for name, center in (("pic0", 2700), ("pic1", 1920)):
        picture = background(center)
        write_dds(os.path.join(OUT, name + ".dds"), picture)
        Image.fromarray(picture.pixels, "RGB").resize((1920, 1080), Image.NEAREST).save(
            os.path.join(OUT, name + "-preview.png"))
        print("wrote %s.dds and %s-preview.png" % (name, name))


if __name__ == "__main__":
    sys.exit(main())
