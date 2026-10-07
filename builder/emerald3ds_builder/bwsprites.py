"""Black/White animated sprites for the 3DS (3ds_port/include/3ds_bw.h).

The sprites are not part of this repository or of the player's ROM: the
builder downloads the animated GIFs from PokeAPI/sprites, converts them as the
PC port's sprite tool does (16 colours, a shiny palette, cropped unique frames
and their timings) and packs them into bw_sprites.pack in the PC port's
format. Downloads are kept in a cache, so only the first install fetches them.

Without the pack the game shows its own pictures, so any failure here is a
warning, never an error.
"""

from __future__ import annotations

import os
import struct
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SOURCE_REPO = "PokeAPI/sprites"
SOURCE_COMMIT = "master"
SOURCE_URL = ("https://raw.githubusercontent.com/%s/%s/sprites/pokemon/versions/generation-v/black-white/animated/"
              % (SOURCE_REPO, SOURCE_COMMIT))
VARIANTS = {"front": "", "back": "back/", "front_shiny": "shiny/", "back_shiny": "back/shiny/"}
PACK_NAME = "bw_sprites.pack"
PACK_MAGIC = b"BWSP"
PACK_VERSION = 1


def sprite_names() -> list[str]:
    """The sprites Emerald needs, in dex order (PokeAPI's file names)."""
    names = []
    for dex in range(1, 387):
        if dex == 201:  # Unown: A-Z, !, ?
            names.append("201")
            names += ["201-" + c for c in "bcdefghijklmnopqrstuvwxyz"]
            names += ["201-exclamation", "201-question"]
        elif dex == 351:  # Castform: normal and the weather forms
            names += ["351", "351-sunny", "351-rainy", "351-snowy"]
        elif dex == 386:  # Emerald's Deoxys is the Speed Forme
            names.append("386-speed")
        else:
            names.append(str(dex))
    return names


def name_key(name: str) -> tuple[int, int]:
    """(national dex number, form)."""
    dex, _, form = name.partition("-")
    forms = {
        "201": ["", *"bcdefghijklmnopqrstuvwxyz", "exclamation", "question"],
        "351": ["", "sunny", "rainy", "snowy"],
        "386": ["speed"],
    }
    return int(dex), forms[dex].index(form) if dex in forms else 0


def default_cache() -> Path:
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base) / "emerald3ds-builder" / "bw"


# ---------------------------------------------------------------------------
# download

def download(cache: Path, progress=None) -> None:
    """Fetches every sprite's four GIFs into `cache` (those not there already)."""
    jobs = []
    for variant, sub in VARIANTS.items():
        for name in sprite_names():
            path = cache / variant / (name + ".gif")
            if not path.exists():
                jobs.append((SOURCE_URL + sub + name + ".gif", path))
    if not jobs:
        return
    done = [0]

    def get(job):
        url, path = job
        req = urllib.request.Request(url, headers={"User-Agent": "emerald3ds-builder"})
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read()
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(path.suffix + ".part")
        tmp.write_bytes(data)
        os.replace(tmp, path)
        done[0] += 1
        if progress:
            progress(done[0] / len(jobs), "Downloading the Black/White sprites")

    with ThreadPoolExecutor(16) as pool:
        list(pool.map(get, jobs))


# ---------------------------------------------------------------------------
# convert (as the PC port's tools/bw_sprites/bw_sprites.py convert_one)

def decode_gif(path: Path):
    """(width, height, frames): each frame the whole canvas as (r, g, b), or
    None where transparent, with its delay in 1/100 s."""
    from PIL import Image, ImageSequence  # the builder ships Pillow

    with Image.open(path) as img:
        w, h = img.size
        frames = []
        for frame in ImageSequence.Iterator(img):
            rgba = frame.convert("RGBA")
            px = [None if a < 128 else (r, g, b) for r, g, b, a in rgba.getdata()]
            frames.append((px, frame.info.get("duration", 100) // 10))
    return w, h, frames


def to555(c):
    """The colour as the GBA shows it (5 bits per channel, expanded back)."""
    return tuple(((v >> 3) << 3) | (v >> 5) for v in c)


def reduce_colors(counts, limit=15):
    """Maps each colour to one of at most `limit` colours: the rarest colours
    merge into their nearest neighbour (a few rips have stray blended pixels)."""
    reps = {c: c for c in counts}
    weight = dict(counts)
    while len(weight) > limit:
        rare = min(weight, key=lambda c: weight[c])
        near = min((c for c in weight if c != rare), key=lambda c: sum((a - b) ** 2 for a, b in zip(c, rare)))
        weight[near] += weight.pop(rare)
        for c, r in reps.items():
            if r == rare:
                reps[c] = near
    return reps, weight


def cropped(px, w):
    """A frame cut down to its opaque area: (shape key, pixels)."""
    xs = [i % w for i, c in enumerate(px) if c is not None]
    ys = [i // w for i, c in enumerate(px) if c is not None]
    if not xs:
        return (0, 0, b""), []
    x0, x1, y0, y1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
    pixels = [px[y * w + x] for y in range(y0, y1) for x in range(x0, x1)]
    return (x1 - x0, y1 - y0, bytes(c is not None for c in pixels)), pixels


def convert_side(cache: Path, name: str, side: str) -> dict:
    """One side of a sprite: its canvas, crop, frame sequence, unique frames
    (rows of palette indices) and normal and shiny palettes."""
    w, h, frames = decode_gif(cache / side / (name + ".gif"))
    sw, _, shiny = decode_gif(cache / (side + "_shiny") / (name + ".gif"))

    # Palette: the colours by frequency (index 0 is transparent).
    counts = {}
    for px, _ in frames:
        for c in px:
            if c is not None:
                c = to555(c)
                counts[c] = counts.get(c, 0) + 1
    reps, weight = reduce_colors(counts)
    colors = sorted(weight, key=lambda c: -weight[c])
    palette = [(0, 0, 0)] + colors + [(0, 0, 0)] * (15 - len(colors))
    index = {c: colors.index(reps[c]) + 1 for c in counts}

    # Shiny palette: whichever shiny colour each normal colour turns into most
    # often, pixel for pixel, over frames of identical shape (failing that,
    # frames at the same point of the animation, or row by row).
    votes = {}
    normal_c = [cropped(px, w) for px, _ in frames]
    shiny_c = [cropped(spx, sw) for spx, _ in shiny]
    shiny_by_shape = {}
    for key, pixels in shiny_c:
        shiny_by_shape.setdefault(key, pixels)
    pairs = [(pixels, shiny_by_shape[key]) for key, pixels in normal_c if key in shiny_by_shape]
    if not pairs:
        for i, (key, pixels) in enumerate(normal_c):
            skey, spixels = shiny_c[i * len(shiny_c) // len(normal_c)]
            if key[:2] == skey[:2]:
                pairs.append((pixels, spixels))
    if not pairs:
        for key, pixels in normal_c:
            skey, spixels = min(shiny_c, key=lambda s: abs(s[0][0] - key[0]) + abs(s[0][1] - key[1]))
            for y in range(min(key[1], skey[1])):
                for x in range(min(key[0], skey[0])):
                    pairs.append(([pixels[y * key[0] + x]], [spixels[y * skey[0] + x]]))
    if not pairs:
        raise ValueError("%s %s: no shiny frame matches a normal one" % (name, side))
    for px, spx in pairs:
        for c, sc in zip(px, spx):
            if c is not None and sc is not None:
                key = (index[to555(c)], to555(sc))
                votes[key] = votes.get(key, 0) + 1
    shiny_pal = list(palette)
    best = {}
    for (i, sc), n in votes.items():
        if n > best.get(i, (0, None))[0]:
            best[i] = (n, sc)
    for i, (_, sc) in best.items():
        shiny_pal[i] = sc

    # Crop to the area any frame uses; keep each distinct frame once.
    xs = [i % w for px, _ in frames for i, c in enumerate(px) if c is not None]
    ys = [i // w for px, _ in frames for i, c in enumerate(px) if c is not None]
    x0, x1, y0, y1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
    uniques, seq = [], []
    for px, delay in frames:
        img = tuple(tuple(index[to555(px[y * w + x])] if px[y * w + x] is not None else 0
                          for x in range(x0, x1)) for y in range(y0, y1))
        if img not in uniques:
            uniques.append(img)
        # GIF delays are in 1/100 s; browsers treat 0-1 as 10.
        seq.append([uniques.index(img), delay if delay > 1 else 10])
    return {"canvas": (w, h), "crop": (x0, y0, x1 - x0, y1 - y0), "seq": seq, "frames": uniques,
            "palette": palette, "shiny": shiny_pal}


# ---------------------------------------------------------------------------
# pack

def to_rgb555(c) -> int:
    return (c[0] >> 3) | ((c[1] >> 3) << 5) | ((c[2] >> 3) << 10)


def lz77_compress(data: bytes) -> bytes:
    """GBA BIOS LZ77 (type 0x10). Greedy, against a run of the byte before or
    the latest earlier occurrence only: the frames are differences from the
    frame before, mostly zeros."""
    out = bytearray(struct.pack("<I", 0x10 | (len(data) << 8)))
    last = {}
    pos, n = 0, len(data)
    while pos < n:
        flag_pos = len(out)
        out.append(0)
        for bit in range(8):
            if pos >= n:
                break
            best_len, best_disp = 0, 0
            for cand in ((pos - 1) if pos else None, last.get(data[pos:pos + 3])):
                if cand is None or pos - cand > 4096 or pos - cand < 1:
                    continue
                length = 0
                while length < 18 and pos + length < n and data[cand + length] == data[pos + length]:
                    length += 1
                if length > best_len:
                    best_len, best_disp = length, pos - cand
            step = best_len if best_len >= 3 else 1
            if best_len >= 3:
                out[flag_pos] |= 0x80 >> bit
                v = ((best_len - 3) << 12) | (best_disp - 1)
                out += bytes([v >> 8, v & 0xFF])
            else:
                out.append(data[pos])
            for p in range(pos, pos + step):
                if p + 3 <= n:
                    last[data[p:p + 3]] = p
            pos += step
    while len(out) % 4:
        out.append(0)
    return bytes(out)


def pack_one(cache: Path, name: str) -> list[dict]:
    result = []
    for side_index, side in enumerate(("front", "back")):
        conv = convert_side(cache, name, side)
        cx, cy, w, h = conv["crop"]
        frames, prev = [], None
        for img in conv["frames"]:
            packed = bytearray()
            for row in img:
                row = list(row) + [0]
                packed += bytes(row[x] | (row[x + 1] << 4) for x in range(0, w, 2))
            # Each frame is stored as the difference from the one before.
            delta = packed if prev is None else bytes(a ^ b for a, b in zip(packed, prev))
            frames.append(lz77_compress(bytes(delta)))
            prev = packed
        dex, form = name_key(name)
        result.append({
            "dex": dex, "form": form, "side": side_index,
            "crop": (cx, cy, w, h), "canvas": conv["canvas"],
            "palettes": [to_rgb555(c) for c in conv["palette"][:16]] + [to_rgb555(c) for c in conv["shiny"][:16]],
            "seq": conv["seq"], "frames": frames,
        })
    return result


def write_pack(cache: Path, out: Path, progress=None) -> dict:
    """bw_sprites.pack from the downloaded sprites (layout: 3ds_port/src/3ds_bw.c)."""
    names = sprite_names()
    sprites = []
    for i, name in enumerate(names):
        sprites += pack_one(cache, name)
        if progress:
            progress((i + 1) / len(names), "Converting the Black/White sprites")
    sprites.sort(key=lambda s: (s["dex"], s["form"], s["side"]))

    entry_format = "<HBBHHHHHHHHIII"
    entry_size = struct.calcsize(entry_format)
    blob = bytearray()
    base = 16 + entry_size * len(sprites)
    entries = bytearray()
    for s in sprites:
        pal_off = base + len(blob)
        blob += struct.pack("<32H", *s["palettes"])
        seq_off = base + len(blob)
        for idx, dur in s["seq"]:
            blob += struct.pack("<HH", idx, dur)
        table_off = base + len(blob)
        blob += bytes(4 * len(s["frames"]))
        for k, data in enumerate(s["frames"]):
            while len(blob) % 4:
                blob.append(0)
            struct.pack_into("<I", blob, table_off - base + 4 * k, base + len(blob))
            blob += data
        while len(blob) % 4:
            blob.append(0)
        cx, cy, w, h = s["crop"]
        entries += struct.pack(entry_format, s["dex"], s["form"], s["side"], s["canvas"][0], s["canvas"][1],
                               cx, cy, w, h, len(s["frames"]), len(s["seq"]), pal_off, seq_off, table_off)
    tmp = out.with_suffix(out.suffix + ".tmp")
    with tmp.open("wb") as f:
        f.write(PACK_MAGIC + struct.pack("<III", PACK_VERSION, len(sprites), 0))
        f.write(entries)
        f.write(blob)
    os.replace(tmp, out)
    return {"sprites": len(sprites), "bytes": out.stat().st_size}


def make_pack(out: Path, cache: Path | None = None, progress=None) -> dict:
    """Downloads (if need be), converts and packs the sprites into `out`. The
    pack is kept in the cache too, so a later install only copies it."""
    import shutil

    cache = Path(cache) if cache else default_cache()
    cache.mkdir(parents=True, exist_ok=True)
    cached = cache / ("bw_sprites-v%d.pack" % PACK_VERSION)
    if not cached.exists():
        download(cache, progress and (lambda f, m: progress(f * 0.3, m)))
        write_pack(cache, cached, progress and (lambda f, m: progress(0.3 + f * 0.7, m)))
    shutil.copy2(cached, out)
    if progress:
        progress(1.0, "Black/White sprites ready")
    return {"bytes": Path(out).stat().st_size}
