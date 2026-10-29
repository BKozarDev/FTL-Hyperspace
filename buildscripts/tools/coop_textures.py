"""Generates the textures of the co-op first-person crew view.

Writes PNGs to "Mod Files/img/coop/" (they ship inside Hyperspace.ftl).
Pure Python, no dependencies: python buildscripts/tools/coop_textures.py
"""

import math
import os
import random
import struct
import zlib

OUT = os.path.join(os.path.dirname(__file__), "..", "..", "Mod Files", "img", "coop")


def write_png(path, w, h, pixels):
    """pixels: list of rows, each a list of (r, g, b, a) in 0..255"""
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in pixels)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def clamp(v):
    return max(0, min(255, int(round(v))))


class Noise:
    """Tileable value noise"""

    def __init__(self, seed, period):
        rng = random.Random(seed)
        self.period = period
        self.grid = [[rng.random() for _ in range(period)] for _ in range(period)]

    def at(self, x, y):
        p = self.period
        x0, y0 = int(math.floor(x)) % p, int(math.floor(y)) % p
        x1, y1 = (x0 + 1) % p, (y0 + 1) % p
        fx, fy = x - math.floor(x), y - math.floor(y)
        fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
        a = self.grid[y0][x0] * (1 - fx) + self.grid[y0][x1] * fx
        b = self.grid[y1][x0] * (1 - fx) + self.grid[y1][x1] * fx
        return a * (1 - fy) + b * fy

    def fbm(self, x, y, size):
        total, amp, freq, norm = 0.0, 1.0, self.period / size, 0.0
        for _ in range(4):
            total += self.at(x * freq, y * freq) * amp
            norm += amp
            amp *= 0.5
            freq *= 2
        return total / norm


def image(w, h, fn):
    return [[tuple(clamp(c) for c in fn(x, y)) for x in range(w)] for y in range(h)]


def metal(n, x, y, size, base, var):
    v = n.fbm(x, y, size)
    return [c + (v - 0.5) * var for c in base]


def wall(size=128):
    n = Noise(1, 16)
    rivets = [(8, 8), (size - 9, 8), (8, size // 3 - 8), (size - 9, size // 3 - 8)]

    def px(x, y):
        c = metal(n, x, y, size, (92, 102, 114), 34)
        # Upper and lower panels with a seam between them
        seam_y = size // 3
        if abs(y - seam_y) <= 1 or x <= 1 or x >= size - 2:
            c = [v * 0.45 for v in c]
        elif abs(y - seam_y) == 2:
            c = [v * 1.18 for v in c]
        # Vertical seam in the middle of the lower panel
        if y > seam_y and abs(x - size // 2) <= 1:
            c = [v * 0.5 for v in c]
        # Recessed vent strip in the upper panel
        if 18 <= y <= seam_y - 14 and 24 <= x <= size - 25 and (x // 4) % 2 == 0:
            c = [v * 0.55 for v in c]
        # Baseboard
        if y >= size - 12:
            c = [v * 0.6 for v in c]
            if y == size - 12:
                c = [v * 1.6 for v in c]
        # Rivets
        for rx, ry in rivets:
            d = math.hypot(x - rx, y - ry)
            if d < 2.6:
                c = [v * (1.5 - d * 0.25) for v in c]
        return c + [255]

    return image(size, size, px)


def wall_outer(size=128):
    n = Noise(2, 16)

    def px(x, y):
        c = metal(n, x, y, size, (70, 74, 82), 30)
        # Horizontal ribs of the hull frame
        rib = y % 32
        if rib < 6:
            shade = 1.25 - abs(rib - 3) * 0.12
            c = [v * shade for v in c]
        elif rib == 6:
            c = [v * 0.45 for v in c]
        # Vertical frame member
        if abs(x - size // 2) < 7:
            c = [v * (1.12 - abs(x - size // 2) * 0.03) for v in c]
        if abs(x - size // 2) == 7:
            c = [v * 0.5 for v in c]
        # Pipes along the bottom
        for py, r in ((size - 20, 4), (size - 9, 3)):
            d = abs(y - py)
            if d <= r:
                c = [v * 0.7 + 40 * (1 - d / (r + 1)) for v in c]
        return c + [255]

    return image(size, size, px)


def floor(size=128):
    n = Noise(3, 16)

    def px(x, y):
        c = metal(n, x, y, size, (64, 68, 72), 26)
        # Tile border
        if x < 3 or y < 3 or x >= size - 3 or y >= size - 3:
            edge = min(x, y, size - 1 - x, size - 1 - y)
            c = [v * (0.55 + edge * 0.12) for v in c]
        else:
            # Grating slots
            gx, gy = (x - 3) % 12, (y - 3) % 12
            if 2 <= gx <= 9 and 4 <= gy <= 7:
                c = [v * 0.3 for v in c]
            elif 2 <= gx <= 9 and gy == 3:
                c = [v * 1.25 for v in c]
        return c + [255]

    return image(size, size, px)


def ceiling(size=128):
    n = Noise(4, 16)

    def px(x, y):
        c = metal(n, x, y, size, (46, 50, 58), 22)
        if x % 64 < 2 or y % 64 < 2:
            c = [v * 0.5 for v in c]
        # Round vent in each quarter
        for cx, cy in ((32, 32), (96, 96)):
            d = math.hypot(x - cx, y - cy)
            if d < 14 and (int(d) % 3 == 0):
                c = [v * 0.4 for v in c]
            elif 14 <= d < 16:
                c = [v * 1.3 for v in c]
        return c + [255]

    return image(size, size, px)


def door(size=128):
    n = Noise(5, 16)

    def px(x, y):
        c = metal(n, x, y, size, (150, 140, 112), 28)
        # Hazard stripes along the sides
        if x < 14 or x >= size - 14:
            c = (225, 180, 40) if ((x + y) // 10) % 2 == 0 else (30, 30, 30)
            c = [v * (0.8 + n.at(x / 8, y / 8) * 0.3) for v in c]
        # Centre seam of the two leaves
        if abs(x - size // 2) <= 1:
            c = [v * 0.35 for v in c]
        # Window slit
        if 40 <= y <= 56 and 30 <= x <= size - 31 and abs(x - size // 2) > 2:
            c = [40, 70, 90]
        # Panel lines
        if y in (24, 25, 100, 101) and 14 <= x < size - 14:
            c = [v * 0.55 for v in c]
        return c + [255]

    return image(size, size, px)


def console(size=64):
    rng = random.Random(6)
    bars = [rng.randint(8, 40) for _ in range(6)]

    def px(x, y):
        c = [10, 24, 22]
        if x < 3 or y < 3 or x >= size - 3 or y >= size - 3:
            return [60, 70, 76, 255]
        # Scan lines and bar graph
        if y % 3 == 0:
            c = [v * 0.7 for v in c]
        col = (x - 8) // 8
        if 0 <= col < 6 and (x - 8) % 8 < 5 and y > size - 12 - bars[col]:
            c = [40, 200, 120]
        if 8 <= y <= 10 and 8 <= x <= size - 9:
            c = [120, 230, 200]
        return c + [255]

    return image(size, size, px)


def light(w=64, h=32):
    def px(x, y):
        dx = abs(x - (w - 1) / 2) / (w / 2)
        dy = abs(y - (h - 1) / 2) / (h / 2)
        inner = dx < 0.8 and dy < 0.55
        if inner:
            return (255, 250, 235, 255)
        d = max(dx, dy)
        a = max(0.0, 1 - (d - 0.6) / 0.4)
        return (255, 240, 210, 255 * a * a)

    return image(w, h, px)


def glow(size=64):
    def px(x, y):
        d = math.hypot(x - (size - 1) / 2, y - (size - 1) / 2) / (size / 2)
        a = max(0.0, 1 - d)
        return (255, 255, 255, 255 * a * a)

    return image(size, size, px)


def white(size=8):
    return image(size, size, lambda x, y: (255, 255, 255, 255))


def main():
    os.makedirs(OUT, exist_ok=True)
    textures = {
        "wall.png": wall(),
        "wall_outer.png": wall_outer(),
        "floor.png": floor(),
        "ceiling.png": ceiling(),
        "door.png": door(),
        "console.png": console(),
        "light.png": light(),
        "glow.png": glow(),
        "white.png": white(),
    }
    for name, pixels in textures.items():
        write_png(os.path.join(OUT, name), len(pixels[0]), len(pixels), pixels)
        print("wrote", name)


if __name__ == "__main__":
    main()
