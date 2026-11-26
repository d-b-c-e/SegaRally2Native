"""Wheelkit (modified version, not upstream's): the letters of the arcade's own screens that upstream's high-resolution
text (tools/make_text_atlas.py) does not cover yet, redrawn with free (SIL OFL / Apache) vector fonts: RANKING's names,
times and places, INSERT COIN(S), CREDIT and the copyright line, the cars' specifications on SELECT CAR.

Writes textures/text_wheelkit/atlas.png and glyphs.txt in upstream's format (src/video/text_hd.h), which TextHD::add puts
below upstream's own atlas when Wheelkit's Redrawn letters option is on. Every tile is an 8 bpp pattern known by the hash
of its pixels ("B" lines), drawn from the glyph it belongs to: a letter of a font set (one size for the set, centred on
the original letter), a word or a line stretched over the original's ink, or a block of lines (each line on its own ink).
Tiles upstream's atlas already has are left out. Colours stay the game's: the gradient's top and bottom and the edge are
palette entries of the tile's bank, read from the original pixels.

Usage: python tools/make_text_screens.py <snapshot folder> [--preview out/text_screens.png]
The snapshots (F9 or LW_SNAP_AT=frame,every,count, out/snap/snap_<frame>.bin) are named in GLYPHS below; the ranking's
names and times were set with SR2_POKE (every letter and digit on screen once). Needs Pillow and numpy.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lwhash  # noqa: E402

FONTS = "third_party/fonts"
OUT = "textures/text_wheelkit"
SCALE, SS, PAD, COLS = 8, 4, 1, 16
SLOT = (8 + 2 * PAD) * SCALE
VRAM_SIZE, LINE_SCROLL, NAME_TABLES, PALETTE, REGS = 0x190000, 0xF6000, 0xF8000, 0x100000, 0x180000

args = sys.argv[1:]
preview = None
if "--preview" in args:
    k = args.index("--preview"); preview = args[k + 1]; del args[k:k + 2]
snap_dir = args[0]
vrams = {}


def vram_of(snap):
    if snap not in vrams:
        vrams[snap] = open(os.path.join(snap_dir, f"snap_{snap}.bin"), "rb").read()[-VRAM_SIZE:]
    return vrams[snap]


def le32(v, o):
    return struct.unpack_from("<I", v, o)[0]


def entry(v, layer, cx, cy):
    """Name table entry shown at screen cell (cx, cy) by a layer (its scroll, or its line scroll, on the 8x8 grid)"""
    sc = le32(v, REGS + 0x60 + 4 * layer)
    xs, ys = sc & 0x3FF, (sc >> 16) & 0x1FF
    if sc & 0x8000:
        w = le32(v, LINE_SCROLL + 0x400 * layer + (cy * 8 // 2) * 4)
        xs = w >> 16 & 0x3FF
    px, py = (cx * 8 + xs) & 511, (cy * 8 + ys) & 511
    w = le32(v, NAME_TABLES + 0x2000 * layer + ((py // 8) * 32 + px // 16) * 4)
    return (w & 0xFFFF) if (px & 8) else (w >> 16)


def tile_bytes(v, e):
    start = ((e & 0x3FFF) * 64) % LINE_SCROLL
    return v[start:start + 64]


def tile(v, e):
    d = tile_bytes(v, e)
    return np.array([[d[y * 8 + (x & 4) + 3 - (x & 3)] for x in range(8)] for y in range(8)])


def palette_rgb(v, i):
    c = le32(v, PALETTE + i * 4)
    return (c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, ((c >> 10) & 31) * 255 // 31


def luma(v, i):
    r, g, b = palette_rgb(v, i)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


# ---- What is redrawn -----------------------------------------------------------------------------------------------
# Each glyph: (snapshot, set, text, (cell x, cell y, width, height)); layer A (0) unless the set says otherwise.
# A set: font, weight, edge (outline | shadow), fit (set: letters of one size; box: the ink stretched over the
# original's; lines: one line of text per run of ink rows, top to bottom, texts separated by "|").
SETS = {
    "rank_letter": {"font": "Exo2-Italic[wght].ttf", "weight": 800, "edge": "outline", "fit": "set", "outline": 1.0,
                    "max_width": 0.95, "same_roles": True, "boxed": "Q"},   # (a Q's tail reaches below: fitted whole)
    "rank_digit": {"font": "Exo2-Italic[wght].ttf", "weight": 800, "edge": "outline", "fit": "set", "outline": 1.0,
                   "max_width": 0.9, "same_roles": True},
    "rank_mark": {"font": "Exo2-Italic[wght].ttf", "weight": 800, "edge": "outline", "fit": "box", "outline": 1.0,
                  "same_roles": "rank_digit"},
    "rank_place": {"font": "Exo2-Italic[wght].ttf", "weight": 800, "edge": "outline", "fit": "box", "letters": True,
                   "outline": 1.0, "block": True},
    # words whose tiles other words share (the C of COIN and CREDIT): drawn as whole blocks ("G8"), each cell its own
    "coin": {"font": "Arimo[wght].ttf", "weight": 700, "edge": "shadow", "fit": "box", "block": True, "flat": True},
    # each car's own picture of its specifications: blocks, so another car's (its labels' tiles are the same) stays
    "specs": {"font": "Arimo[wght].ttf", "weight": 700, "edge": "outline", "fit": "lines", "words": True, "flat": True,
              "outline": 0.6, "block": True},
}
GLYPHS = []   # filled from glyphs_screens.txt beside this tool (one glyph a line: snap set cx cy w h text)
for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "glyphs_screens.txt"), encoding="utf-8"):
    line = line.rstrip("\n")
    if not line.strip() or line.startswith("#"):
        continue
    snap, set_name, cx, cy, w, h, text = line.split(" ", 6)
    GLYPHS.append((snap, set_name, text, (int(cx), int(cy), int(w), int(h))))


def original(g):
    snap, _, _, (cx, cy, w, h) = g
    v = vram_of(snap)
    es = [[entry(v, 0, cx + x, cy + y) for x in range(w)] for y in range(h)]
    img = np.vstack([np.hstack([tile(v, e) for e in r]) for r in es])
    first = next((e for r in es for e in r if any(tile_bytes(v, e))), es[0][0])
    return v, es, img, first & 0x7F00


def roles(v, img, base, style):
    """The edge (the darkest colour) and the gradient's top and bottom: the commonest bright colour in the top and in
    the bottom quarter of each letter row's height (rows of ink taken one run at a time)"""
    used = [int(n) for n in np.unique(img) if n]
    edge = min(used, key=lambda n: luma(v, base | n)) if len(used) > 1 else 0
    if not edge:
        return used[0], used[0], 0
    lum = np.vectorize(lambda n: luma(v, base | int(n)) if n else -1.0)(img)
    body_l = [luma(v, base | n) for n in used if n != edge]
    cut = (luma(v, base | edge) + min(body_l)) / 2 if body_l else 0
    bright = (img != 0) & (lum > max(cut, (luma(v, base | edge) + max(body_l)) / 2 - 1e-6) * 0 + cut)
    tops, bottoms = [], []
    rows = bright.any(axis=1)
    y = 0
    while y < len(rows):
        if rows[y]:
            y0 = y
            while y < len(rows) and rows[y]:
                y += 1
            q = max(1, (y - y0) // 4)
            tops += list(img[y0:y0 + q][bright[y0:y0 + q]])
            bottoms += list(img[y - q:y][bright[y - q:y]])
        else:
            y += 1
    common = lambda xs: int(max(set(xs), key=xs.count)) if xs else used[-1]
    return common(tops), common(bottoms), edge


def load_font(s, size):
    f = ImageFont.truetype(os.path.join(FONTS, s["font"]), max(1, int(round(size))))
    if s.get("weight"):
        try:
            f.set_variation_by_axes([s["weight"]])
        except OSError:
            pass
    return f


def ink(s, text, size):
    f = load_font(s, size)
    bb = f.getbbox(text, anchor="ls")
    pad = int(size)
    layer = Image.new("L", (bb[2] - bb[0] + 2 * pad, bb[3] - bb[1] + 2 * pad), 0)
    ImageDraw.Draw(layer).text((pad - bb[0], pad - bb[1]), text, font=f, fill=255, anchor="ls")
    a = np.asarray(layer, np.float32) / 255
    ys, xs = np.nonzero(a > 0.02)
    return a, (xs.min(), ys.min(), xs.max() + 1, ys.max() + 1), (pad - bb[0], pad - bb[1])


def stretch(a, w, h):
    return np.asarray(Image.fromarray((a * 255).astype(np.uint8)).resize((max(1, w), max(1, h)), Image.LANCZOS),
                      np.float32) / 255


def dilate(m, r):
    out = m.copy()
    ir = int(np.ceil(r))
    for dy in range(-ir, ir + 1):
        for dx in range(-ir, ir + 1):
            d = np.hypot(dx, dy)
            if d <= r + 0.5:
                out = np.maximum(out, min(1.0, r + 0.5 - d) * np.roll(np.roll(m, dy, 0), dx, 1))
    return out


def body_box(mask):
    ys, xs = np.nonzero(mask)
    return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1


# One size per "set" set: the capitals' and digits' height in the originals (the tallest body), and one baseline (the
# commonest bottom of the letters and digits, from the top of their cells: a Q's tail or a mark does not move it)
set_cap, set_bottoms = {}, {}
for g in GLYPHS:
    s = SETS[g[1]]
    if s["fit"] != "set":
        continue
    v, es, img, base = original(g)
    top, bottom, edge = roles(v, img, base, s["edge"])
    x0, y0, x1, y1 = body_box((img != 0) & (img != edge))
    if g[2].isalnum() and g[2] != "Q":
        set_cap[g[1]] = max(set_cap.get(g[1], 0), y1 - y0)
        set_bottoms.setdefault(g[1], []).append(y1)
set_base = {k: int(np.median(v)) for k, v in set_bottoms.items()}

K = SCALE * SS
set_roles = {}
blocks = []   # "G8" blocks: (w, h, roles, [(hash, tile image)])
entries, order, dupes = {}, [], 0
previews = []
for g in GLYPHS:
    snap, set_name, text, (cx, cy, w, h) = g
    s = SETS[set_name]
    v, es, img, base = original(g)
    top, bottom, edge = roles(v, img, base, s["edge"])
    if s.get("same_roles"):   # the set's first glyph's colours for every glyph of the same palette bank
        key_ = s["same_roles"] if isinstance(s["same_roles"], str) else set_name
        top, bottom, edge = set_roles.setdefault((key_, base), (top, bottom, edge))
    if s.get("flat"):   # one colour: the brighter of the two
        top = bottom = max((top, bottom), key=lambda n: luma(v, base | n))
    # the letters' own pixels: brighter than halfway from the edge to the body's darker end (the outline's and the
    # shadow's shades left out)
    lum = np.vectorize(lambda n: luma(v, base | int(n)) if n else -1.0)(img)
    cut = (luma(v, base | edge) + min(luma(v, base | top), luma(v, base | bottom))) / 2 if edge else 0
    body = (img != 0) & (lum > cut)
    W, H = (w * 8 + 2 * PAD) * K, (h * 8 + 2 * PAD) * K
    canvas = np.zeros((H, W), np.float32)
    grad = np.zeros((H, W), np.float32)
    def letter_boxes(t, x0, y0, x1, y1):
        """Each character of t (spaces left out) on its own run of ink columns, its box that letter's ink; None when
        the runs and the characters do not pair up (letters that touch: the whole is stretched instead)"""
        cols = body[y0:y1, x0:x1].any(axis=0)
        runs, x = [], 0
        while x < len(cols):
            if cols[x]:
                a0 = x
                while x < len(cols) and cols[x]:
                    x += 1
                runs.append((a0, x))
            else:
                x += 1
        chars = [c for c in t if c != " "]
        if len(runs) != len(chars):
            return None
        out = []
        for c, (a0, a1) in zip(chars, runs):
            ys_ = np.nonzero(body[y0:y1, x0 + a0:x0 + a1].any(axis=1))[0]
            out.append((c, (x0 + a0, y0 + ys_.min(), x0 + a1, y0 + ys_.max() + 1)))
        return out

    if s["fit"] == "lines":
        rows = body.any(axis=1)
        runs, y = [], 0
        while y < len(rows):
            if rows[y]:
                y0 = y
                while y < len(rows) and rows[y]:
                    y += 1
                runs.append((y0, y))
            else:
                y += 1
        texts = text.split("|")
        while len(runs) < len(texts):   # lines that touch (a descender on the next line's capitals): cut the
            i = max(range(len(runs)), key=lambda k: runs[k][1] - runs[k][0])   # tallest at its thinnest row
            y0, y1 = runs[i]
            ink_rows = body[y0 + 2:y1 - 2].sum(axis=1)
            if len(ink_rows) == 0:
                break
            cut_at = y0 + 2 + int(np.argmin(ink_rows))
            runs[i:i + 1] = [(y0, cut_at), (cut_at + 1, y1)] if ink_rows.min() == 0 else [(y0, cut_at), (cut_at, y1)]
        if len(runs) != len(texts):
            sys.exit(f"{snap} {set_name} {text!r}: {len(runs)} lines of ink, {len(texts)} texts")
        boxes = []
        for (y0, y1), t in zip(runs, texts):
            xs = np.nonzero(body[y0:y1].any(axis=0))[0]
            boxes.append((t, (xs.min(), y0, xs.max() + 1, y1)))
    else:
        boxes = [(text, body_box(body))]
    def word_boxes(t, x0, y0, x1, y1):
        """The words of t (split at spaces) on the runs of ink between gaps of 2 or more empty columns"""
        cols = body[y0:y1, x0:x1].any(axis=0)
        runs, x, gap = [], 0, 0
        for x in range(len(cols)):
            if cols[x]:
                if runs and gap < 2:
                    runs[-1][1] = x + 1
                else:
                    runs.append([x, x + 1])
                gap = 0
            else:
                gap += 1
        words = t.split()
        if len(runs) != len(words):
            return [(t, (x0, y0, x1, y1))]
        # the words' widths in the font against the runs': a pairing that is off (two words in one run, a mark
        # alone in another) stretches the whole instead
        probe = load_font(s, 100)
        fw = [probe.getlength(w_) for w_ in words]
        ratios = [(a1 - a0) / max(f_, 1) for (a0, a1), f_ in zip(runs, fw)]
        if max(ratios) > 2.2 * min(ratios):
            return [(t, (x0, y0, x1, y1))]
        out = []
        for w_, (a0, a1) in zip(words, runs):
            ys_ = np.nonzero(body[y0:y1, x0 + a0:x0 + a1].any(axis=1))[0]
            out.append((w_, (x0 + a0, y0 + ys_.min(), x0 + a1, y0 + ys_.max() + 1)))
        return out

    if s.get("words"):   # word by word, each stretched over its own ink
        boxes = [(t, b, (b0[1], b0[3]), None) for t0, b0 in boxes for t, b in word_boxes(t0, *b0)]
    elif s.get("letters"):   # word by word, and letter by letter where the letters stand apart in the original
        split = []
        for t0, b0 in boxes:
            parts = []
            for t, (x0, y0, x1, y1) in word_boxes(t0, *b0):
                lb = letter_boxes(t, x0, y0, x1, y1)
                parts += [(t, (x0, y0, x1, y1), False)] if lb is None else [(c, b, True) for c, b in lb]
            # the line's capitals (and digits): their height and baseline give every letter of the line its size
            caps = [b for c, b, one in parts if one and (c.isupper() or c.isdigit())]
            cap_h = max((b[3] - b[1] for b in caps), default=b0[3] - b0[1])
            baseline = max((b[3] for b in caps), default=b0[3])
            split += [(c, b, (b0[1], b0[3]), (cap_h, baseline) if one else None) for c, b, one in parts]
        boxes = split
    else:
        boxes = [(t, b, (b[1], b[3]), None) for t, b in boxes]
    for t, (x0, y0, x1, y1), (ly0, ly1), line_size in boxes:
        bw, bh = int((x1 - x0) * K), int((y1 - y0) * K)
        if (s["fit"] == "set" and t not in s.get("boxed", "")) or line_size:
            # one capital height (for the set, or the line); the letter centred on the original's, on the baseline
            probe = load_font(s, 1000)
            cap = -probe.getbbox("H", anchor="ls")[1] / 1000.0
            cap_h, base_y = line_size if line_size else (set_cap[set_name], set_base[set_name])
            a, box, origin = ink(s, t, cap_h * K / cap)
            gl = a[box[1]:box[3], box[0]:box[2]]
            gw = gl.shape[1]
            if gw > bw * s.get("max_width", 1.0):
                gl = stretch(gl, int(bw * s.get("max_width", 1.0)), gl.shape[0])
            gx = int((PAD * K) + (x0 + x1) / 2 * K - gl.shape[1] / 2)
            # on the baseline (a Q's tail, descenders and marks where the font puts them)
            gy = int((PAD * K) + base_y * K - (origin[1] - box[1]))
            room = int((PAD + h * 8 - 0.5) * K) - gy   # what still fits above the bottom of the glyph's cells
            if s["fit"] == "set" and gl.shape[0] > room > 0:   # (a Q's tail: the letter raised, then smaller)
                lift = min(gl.shape[0] - room, int(0.3 * cap_h * K), gy)
                gy -= lift
                room += lift
                if gl.shape[0] > room:
                    f_ = room / gl.shape[0]
                    gl = stretch(gl, max(1, int(gl.shape[1] * f_)), room)
                    gx = int((PAD * K) + (x0 + x1) / 2 * K - gl.shape[1] / 2)
        else:
            a, box, _ = ink(s, t, bh * 2)
            gl = stretch(a[box[1]:box[3], box[0]:box[2]], bw, bh)
            gx, gy = int((PAD + x0) * K), int((PAD + y0) * K)
        ys0, xs0 = max(0, -gy), max(0, -gx)
        ys1, xs1 = min(gl.shape[0], H - gy), min(gl.shape[1], W - gx)
        if ys1 <= ys0 or xs1 <= xs0:
            print(f"{snap} {set_name} {t!r}: outside its cells")
            continue
        canvas[gy + ys0:gy + ys1, gx + xs0:gx + xs1] = np.maximum(canvas[gy + ys0:gy + ys1, gx + xs0:gx + xs1],
                                                                   gl[ys0:ys1, xs0:xs1])
        # the gradient over this letter's (or word's) own height, under its own columns and their edge
        if line_size or s["fit"] == "set":
            ly0, ly1 = y0, y1
        yy = (np.arange(H, dtype=np.float32) + 0.5) / K - PAD
        col = np.clip((yy - ly0) / max(ly1 - ly0, 1), 0, 1)
        band = (yy >= ly0 - 1.5) & (yy < ly1 + 1.5)
        gx0, gx1 = max(0, gx - 2 * K), min(W, gx + gl.shape[1] + 2 * K)
        grad[band, gx0:gx1] = col[band, None]
    Wd, Hd = W // SS, H // SS
    letter = canvas.reshape(Hd, SS, Wd, SS).mean(axis=(1, 3))
    gr = grad.reshape(Hd, SS, Wd, SS).mean(axis=(1, 3))
    if s["edge"] == "outline":
        e = dilate(letter, s.get("outline", 0.85) * SCALE)
    else:
        e = np.roll(np.roll(dilate(letter, 0.3 * SCALE), int(0.7 * SCALE), 0), int(0.8 * SCALE), 1)
    big = (np.dstack([letter, e, gr]) * 255 + 0.5).astype(np.uint8)
    targets = [np.array(palette_rgb(v, base | n), np.float32) for n in (top, bottom, edge)]

    def nearest(bank, target):   # the entry of a tile's own palette bank nearest to a colour (banks differ per tile)
        return min(range(1, 256), key=lambda n: float(np.sum((np.array(palette_rgb(v, bank | n), np.float32) - target) ** 2)))

    if s.get("block"):   # TextHD takes blocks of up to 64 cells: bands of whole rows
        band = max(1, 64 // w)
        for by in range(0, h, band):
            cells_ = []
            for ty in range(by, min(h, by + band)):
                for tx in range(w):
                    data = tile_bytes(v, es[ty][tx])
                    cells_.append((lwhash.fnv1a_bytes(data) if any(data) else 0,
                                   big[ty * 8 * SCALE:ty * 8 * SCALE + SLOT, tx * 8 * SCALE:tx * 8 * SCALE + SLOT]))
            if any(c[0] for c in cells_):
                # the game colours a block from its first tile's palette bank: the colours as entries of that bank
                anchor = next(es[ty][tx] for ty in range(by, min(h, by + band)) for tx in range(w)
                              if any(tile_bytes(v, es[ty][tx])))
                bank = anchor & 0x7F00
                rl = (top, bottom, edge) if bank == base else (nearest(bank, targets[0]), nearest(bank, targets[1]),
                                                               nearest(bank, targets[2]) if edge else 0)
                blocks.append((w, min(h, by + band) - by, rl, cells_))
        previews.append((g, img, base, v, big, (top, bottom, edge)))
        continue
    for ty in range(h):
        for tx in range(w):
            ent = es[ty][tx]
            data = tile_bytes(v, ent)
            if not any(data):
                continue
            key = lwhash.fnv1a_bytes(data)
            if key in entries:
                dupes += 1
                continue
            bank = ent & 0x7F00
            rl = (top, bottom, edge) if bank == base else tuple(nearest(bank, t) for t in targets[:2]) + (
                nearest(bank, targets[2]) if edge else 0,)
            entries[key] = (ent & 0x3FFF, big[ty * 8 * SCALE:ty * 8 * SCALE + SLOT, tx * 8 * SCALE:tx * 8 * SCALE + SLOT], rl)
            order.append(key)
    previews.append((g, img, base, v, big, (top, bottom, edge)))

# Tiles upstream's atlas already draws (its "B" lines) are left out
known = set()
for line in open("textures/text/glyphs.txt"):
    p = line.split()
    if p and p[0] == "B":
        known.add(int(p[2], 16))
order = [k for k in order if k not in known]
rows = (len(order) + sum(len(b[3]) for b in blocks) + COLS - 1) // COLS
atlas = np.zeros((rows * SLOT, COLS * SLOT, 4), np.uint8)
atlas[..., 3] = 255
os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "glyphs.txt"), "w") as f:
    f.write("# Wheelkit (modified version, not upstream's): the arcade screens' letters, tools/make_text_screens.py\n")
    f.write("# B pattern hash slot top bottom edge (8 bpp, slots after upstream's atlas: TextHD::add)\n")
    for slot, key in enumerate(order):
        p, t, rl = entries[key]
        sy, sx = divmod(slot, COLS)
        atlas[sy * SLOT:(sy + 1) * SLOT, sx * SLOT:(sx + 1) * SLOT, :3] = t
        f.write(f"B {p:04x} {key:016x} {slot} {rl[0]:x} {rl[1]:x} {rl[2]:x}\n")
    slot = len(order)
    for w, h, rl, cells_ in blocks:   # G8 width height top bottom edge, then hash:slot per cell (hash 0: empty)
        parts = []
        for hh, t in cells_:
            sy, sx = divmod(slot, COLS)
            atlas[sy * SLOT:(sy + 1) * SLOT, sx * SLOT:(sx + 1) * SLOT, :3] = t
            parts.append(f"{hh:016x}:{slot}")
            slot += 1
        f.write(f"G8 {w} {h} {rl[0]:x} {rl[1]:x} {rl[2]:x} " + " ".join(parts) + "\n")
Image.fromarray(atlas, "RGBA").save(os.path.join(OUT, "atlas.png"))
print(f"{OUT}/atlas.png: {len(order)} tiles ({dupes} shared), {len(blocks)} blocks, {COLS * SLOT}x{rows * SLOT}")

if preview:
    lines = []
    for g, img, base, v, big, rl in previews:
        h, w = img.shape
        orig = np.zeros((h * SCALE, w * SCALE, 3), np.float32) + np.array([30, 34, 70], np.float32)
        for yy in range(h):
            for xx in range(w):
                if img[yy, xx]:
                    orig[yy * SCALE:(yy + 1) * SCALE, xx * SCALE:(xx + 1) * SCALE] = palette_rgb(v, base | img[yy, xx])
        px = big[PAD * SCALE:PAD * SCALE + h * SCALE, PAD * SCALE:PAD * SCALE + w * SCALE].astype(np.float32) / 255
        top, bottom, edge = (np.array(palette_rgb(v, base | n), np.float32) for n in rl)
        line = np.zeros_like(orig) + np.array([30, 34, 70], np.float32)
        fill = top * (1 - px[..., 2:3]) + bottom * px[..., 2:3]
        if rl[2]:
            line = line * (1 - px[..., 1:2]) + edge * px[..., 1:2]
        line = line * (1 - px[..., 0:1]) + fill * px[..., 0:1]
        lines.append(np.hstack([orig, line]))
    width = max(l.shape[1] for l in lines)
    sheet = np.vstack([np.pad(l, ((4, 4), (0, width - l.shape[1]), (0, 0))) for l in lines])
    Image.fromarray(sheet.clip(0, 255).astype(np.uint8), "RGB").save(preview)
    print("Preview:", preview)
