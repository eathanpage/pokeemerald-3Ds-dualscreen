"""make_hd_tiles.py TREE OUT: the battle's art upscaled ahead of time, for the
3DS port's 1.5x battle (3ds_video.c, the upscaled art).

Every 4bpp tile of the battle's graphics is upscaled 3x with Scale3x
(AdvMAME3x) in the picture it belongs to, so its edges carry on into its
neighbours as drawn: a background as its tilemap lays it out, a sprite sheet
as the sheet. Scale3x only copies pixels, so the result is still colour
indices - the game's palettes, fades and colour cycling apply to it as to the
original. The 3DS finds a tile's upscaled version by the tile's own 32 bytes
and averages it down to 1.5x as it draws.

The pack (hd_tiles.pack):
  header   "HDTP", u32 version (2), u32 count, u32 fonts (offset of the fonts)
  index    count x {u64 hash, u32 offset, u32 0}, sorted by hash
  entries  at each offset: the tile's 32 bytes (GBA 4bpp), then its 24x24
           upscale as 4bpp, rows of 12 bytes, low nibble first (288 bytes)
  fonts    u32 n, then per font (small, normal, short, narrow, small narrow):
           u32 glyphs, then per glyph its 16x16 cell (128 bytes, 4bpp) and
           its 48x48 upscale (1152 bytes)
The hash is FNV-1a 64 of the 32 bytes.
"""
import os
import struct
import sys

from PIL import Image

TREE = sys.argv[1]
OUT = sys.argv[2]
GFX = os.path.join(TREE, "graphics")

# Whole directories of the battle's art, and backgrounds laid out by tilemaps.
SHEET_DIRS = ["battle_anims", "battle_environment", "battle_interface", "balls", "trainers/front_pics",
              "trainers/back_pics", "battle_transitions", "types"]


def fnv64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def load_indices(path):
    """The picture's colour indices (rows of ints), or None if not a 4bpp picture."""
    try:
        im = Image.open(path)
    except Exception:
        return None
    if im.mode != "P" or im.width % 8 or im.height % 8:
        return None
    w, h = im.size
    px = list(im.getdata())
    if max(px, default=0) > 15:
        return None
    return [px[y * w:(y + 1) * w] for y in range(h)]


def scale3x(img, wrap):
    """AdvMAME3x of rows of values; outside is the opposite edge (wrap) or 0."""
    h, w = len(img), len(img[0])
    out = [[0] * (w * 3) for _ in range(h * 3)]

    def at(x, y):
        if wrap:
            return img[y % h][x % w]
        if x < 0 or y < 0 or x >= w or y >= h:
            return 0
        return img[y][x]

    for y in range(h):
        o0, o1, o2 = out[y * 3], out[y * 3 + 1], out[y * 3 + 2]
        for x in range(w):
            E = img[y][x]
            B, D, F, H = at(x, y - 1), at(x - 1, y), at(x + 1, y), at(x, y + 1)
            x3 = x * 3
            if B != H and D != F:
                A, C, G, I = at(x - 1, y - 1), at(x + 1, y - 1), at(x - 1, y + 1), at(x + 1, y + 1)
                o0[x3] = D if D == B else E
                o0[x3 + 1] = B if (D == B and E != C) or (B == F and E != A) else E
                o0[x3 + 2] = F if B == F else E
                o1[x3] = D if (D == B and E != G) or (D == H and E != A) else E
                o1[x3 + 1] = E
                o1[x3 + 2] = F if (B == F and E != I) or (H == F and E != C) else E
                o2[x3] = D if D == H else E
                o2[x3 + 1] = H if (D == H and E != I) or (H == F and E != G) else E
                o2[x3 + 2] = F if H == F else E
            else:
                o0[x3] = o0[x3 + 1] = o0[x3 + 2] = E
                o1[x3] = o1[x3 + 1] = o1[x3 + 2] = E
                o2[x3] = o2[x3 + 1] = o2[x3 + 2] = E
    return out


def tile_bytes(img, tx, ty):
    """A tile of index rows as GBA 4bpp (low nibble first)."""
    data = bytearray()
    for y in range(8):
        row = img[ty * 8 + y]
        for x in range(0, 8, 2):
            data.append((row[tx * 8 + x] & 15) | ((row[tx * 8 + x + 1] & 15) << 4))
    return bytes(data)


def hd_bytes(big, cx, cy, flipx=False, flipy=False):
    """A cell's 24x24 of the upscaled picture as 4bpp, in its tile's own
    orientation (a flipped map entry flipped back)."""
    data = bytearray()
    for y in range(24):
        sy = cy * 24 + (23 - y if flipy else y)
        row = big[sy]
        for x in range(0, 24, 2):
            a = row[cx * 24 + (23 - x if flipx else x)] & 15
            b = row[cx * 24 + (23 - (x + 1) if flipx else x + 1)] & 15
            data.append(a | (b << 4))
    return bytes(data)


found = {}   # tile bytes -> upscale


def record(key, hd):
    if key not in found and any(key):
        found[key] = hd


def from_tilemap(png, binpath):
    """A background as its tilemap lays it out (screenblocks of 32x32)."""
    img = load_indices(png)
    if img is None or not os.path.exists(binpath):
        return
    raw = open(binpath, "rb").read()
    entries = struct.unpack("<%dH" % (len(raw) // 2), raw[: len(raw) // 2 * 2])
    n = len(entries)
    if n == 2048:
        mw, mh = 64, 32
    elif n % 32 == 0:
        mw, mh = 32, n // 32
    else:
        return
    tw = len(img[0]) // 8
    tiles = tw * (len(img) // 8)
    # Values that tell palettes apart, for Scale3x's comparisons.
    view = [[0] * (mw * 8) for _ in range(mh * 8)]
    cells = {}
    for i, e in enumerate(entries):
        if mw == 64:
            block, rest = divmod(i, 1024)
            cy, cx = rest // 32, rest % 32 + block * 32
        else:
            cy, cx = divmod(i, 32)
        t, fx, fy, pal = e & 1023, (e >> 10) & 1, (e >> 11) & 1, e >> 12
        if t >= tiles:
            continue
        cells[(cx, cy)] = (t, fx, fy)
        for y in range(8):
            sy = (t // tw) * 8 + (7 - y if fy else y)
            for x in range(8):
                v = img[sy][(t % tw) * 8 + (7 - x if fx else x)]
                view[cy * 8 + y][cx * 8 + x] = (pal << 4) | v if v else 0
    big = scale3x(view, wrap=True)
    for (cx, cy), (t, fx, fy) in cells.items():
        record(tile_bytes(img, t % tw, t // tw), hd_bytes(big, cx, cy, fx, fy))


def from_sheet(png):
    img = load_indices(png)
    if img is None:
        return
    big = scale3x(img, wrap=False)
    for ty in range(len(img) // 8):
        for tx in range(len(img[0]) // 8):
            record(tile_bytes(img, tx, ty), hd_bytes(big, tx, ty))


# Backgrounds first: their tilemaps are the right neighbourhood.
for d in ["battle_environment", "battle_anims/backgrounds"]:
    for dirpath, _, files in os.walk(os.path.join(GFX, d)):
        for f in sorted(files):
            if not f.endswith(".bin"):
                continue
            stem = f[:-4]
            for png in (stem + ".png", stem.replace("map", "tiles") + ".png", "tiles.png"):
                if os.path.exists(os.path.join(dirpath, png)):
                    from_tilemap(os.path.join(dirpath, png), os.path.join(dirpath, f))
                    break
for d in SHEET_DIRS:
    for dirpath, _, files in os.walk(os.path.join(GFX, d)):
        for f in sorted(files):
            if f.endswith(".png"):
                from_sheet(os.path.join(dirpath, f))

# The Latin fonts: each glyph's 16x16 cell and its 3x Scale3x, as the values
# of the font's picture (0 the background - the picture's 0 and 3 alike - 1
# and 2 its two inks). The 3DS colours them as the game colours the glyph
# (text.c, CopyGlyphToWindow).
FONTS = ["latin_small", "latin_normal", "latin_short", "latin_narrow", "latin_small_narrow"]


def font_section():
    data = bytearray(struct.pack("<I", len(FONTS)))
    for name in FONTS:
        img = load_indices(os.path.join(GFX, "fonts", name + ".png"))
        cells = (len(img[0]) // 16) * (len(img) // 16)
        data += struct.pack("<I", cells)
        for g in range(cells):
            gx, gy = (g % (len(img[0]) // 16)) * 16, (g // (len(img[0]) // 16)) * 16
            cell = [[0 if img[gy + y][gx + x] == 3 else img[gy + y][gx + x] for x in range(16)] for y in range(16)]
            big = scale3x(cell, wrap=False)
            for rows, w in ((cell, 16), (big, 48)):
                for row in rows:
                    for x in range(0, w, 2):
                        data.append((row[x] & 15) | ((row[x + 1] & 15) << 4))
    return bytes(data)


items = sorted((fnv64(k), k, v) for k, v in found.items())
base = 16 + 16 * len(items)
index = bytearray()
body = bytearray()
for h, k, v in items:
    index += struct.pack("<QII", h, base + len(body), 0)
    body += k + v
fonts = font_section()
header = struct.pack("<4sIII", b"HDTP", 2, len(items), base + len(body))
with open(OUT, "wb") as out:
    out.write(header + index + body + fonts)
print("hd tiles: %d tiles, fonts %d bytes, %d bytes" % (len(items), len(fonts), 16 + len(index) + len(body) + len(fonts)))
