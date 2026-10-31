#!/usr/bin/env python3
"""Generates src/lcd147/art.h: the 1.47" timer's bitmap fonts and the LapDog mark.

The screens (see the "LapDog 1.47" timer display" design) use Chakra Petch for headings,
JetBrains Mono for times and IBM Plex Sans for text, as the app does. They're rendered here
to 4-bit anti-aliased glyph bitmaps, one table per size and character set, so the firmware
needs no font engine. The mark comes from lapdog-web's brand kit, as the touch timer's does:
the dog for the splash (which draws the signal arcs itself, so they can bark) and the whole
mark, small, for the top bar.

    python3 tools/gen-lcd147-art.py ../web/brand ../android/app/src/main/res/font

Needs Pillow. Rerun it when a font size or character set below changes.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
BRAND = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "web", "brand")
FONTS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "..", "..", "android", "app", "src", "main", "res", "font")
OUT = os.path.join(HERE, "..", "src", "lcd147", "art.h")

DIGITS = "0123456789"
UPPER = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
LOWER = UPPER.lower()
ASCII = "".join(chr(c) for c in range(32, 127))
MIDDOT = "·"

# (name, file, px size, characters)
SETS = [
    ("MONO56", "jetbrains_mono_bold", 56, DIGITS + ".:-"),                  # the lap clock
    ("MONO21", "jetbrains_mono_bold", 21, DIGITS + ".:-+"),                 # card values
    ("MONO15", "jetbrains_mono_bold", 15, DIGITS + ".:-+"),
    ("MONO12", "jetbrains_mono_bold", 12, UPPER + DIGITS + " .:+-/" + MIDDOT),  # chips, deltas
    ("MONO10", "jetbrains_mono_bold", 10, "BOTv" + DIGITS + "."),           # BOOT key, version
    ("CHAKRA54", "chakra_petch_bold", 54, UPPER + DIGITS + " !?.-:/"),      # READY, ARM NOW
    ("CHAKRA46", "chakra_petch_bold", 46, UPPER + LOWER + DIGITS + " -_.#"),  # Heat 3
    ("CHAKRA40", "chakra_petch_bold", 40, DIGITS),                          # lap number
    ("CHAKRA26", "chakra_petch_bold", 26, UPPER),                           # splash wordmark
    ("CHAKRA16", "chakra_petch_bold", 16, UPPER + DIGITS + "/"),            # top bar wordmark
    ("CHAKRA15", "chakra_petch_bold", 15, UPPER + " "),                     # ARM bar
    ("CHAKRA13", "chakra_petch_bold", 13, UPPER + " "),                     # NEW BEST
    ("PLEX11", "ibm_plex_sans_regular", 11, ASCII + MIDDOT),
    ("PLEX12", "ibm_plex_sans_regular", 12, ASCII + MIDDOT),
    ("PLEX14", "ibm_plex_sans_regular", 14, ASCII + MIDDOT),
    ("PLEX11B", "ibm_plex_sans_semibold", 11, ASCII + MIDDOT),
    ("PLEX12B", "ibm_plex_sans_semibold", 12, ASCII + MIDDOT),
]


def font_set(name, file, size, chars):
    font = ImageFont.truetype(os.path.join(FONTS, file + ".ttf"), size)
    ascent, descent = font.getmetrics()
    rows = []   # (code, x, y, w, h, advance16, nibbles)
    packed = bytearray()
    for ch in sorted(set(chars)):
        adv = round(font.getlength(ch) * 16)
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
        w, h = x1 - x0, y1 - y0
        if ch == " " or w <= 0 or h <= 0:
            rows.append((ord(ch), 0, 0, 0, 0, adv, len(packed)))
            continue
        im = Image.new("L", (w, h), 0)
        ImageDraw.Draw(im).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
        nib = [min(15, (v + 8) // 17) for v in im.getdata()]  # 0-255 -> 0-15
        if len(nib) % 2:
            nib.append(0)
        off = len(packed)
        packed += bytes((nib[i] << 4) | nib[i + 1] for i in range(0, len(nib), 2))
        rows.append((ord(ch), x0, y0, w, h, adv, off))
    return name, ascent, descent, rows, packed


def wrap(tokens, per_line):
    return ",\n    ".join(", ".join(tokens[i:i + per_line]) for i in range(0, len(tokens), per_line))


def emit_font(name, ascent, descent, rows, packed):
    lo = name.lower()
    s = f"static const uint8_t {lo}_a[{max(1, len(packed))}] = {{\n    " + wrap([f"0x{b:02X}" for b in packed] or ["0x00"], 24) + "\n};\n"
    s += f"static const Glyph {lo}_g[] = {{\n"
    for code, x, y, w, h, adv, off in rows:
        s += f"    {{0x{code:04X}, {x}, {y}, {w}, {h}, {adv}, {off}}},\n"
    s += "};\n"
    s += (f"static const Font {name} = {{{lo}_g, {len(rows)}, {lo}_a, {ascent}, {descent}}};\n")
    return s


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def image(name, im):
    w, h = im.size
    px = list(im.getdata())
    col = [f"0x{rgb565(r, g, b) if a else 0:04X}" for r, g, b, a in px]
    alp = [f"0x{a:02X}" for _, _, _, a in px]
    return (f"constexpr int {name.upper()}_W = {w}, {name.upper()}_H = {h};\n"
            f"static const uint16_t {name}_c[{w * h}] = {{\n    {wrap(col, 16)}\n}};\n"
            f"static const uint8_t {name}_a[{w * h}] = {{\n    {wrap(alp, 24)}\n}};\n")


def resize(im, size):
    # Premultiplied, so transparent edges don't bleed dark fringes.
    return im.convert("RGBa").resize(size, Image.LANCZOS).convert("RGBA")


# Mark geometry, in the brand kit's design units (gen.py): the PNG is MARK_BOX scaled to 512
# wide; everything right of the dog's nose is the signal arcs.
MARK_X, MARK_Y, MARK_W, MARK_H = 27, 3, 97, 89
ARCS_FROM = 102.5
mark = Image.open(os.path.join(BRAND, "png", "lapdog-mark-512.png")).convert("RGBA")
k = mark.width / MARK_W
dog = mark.crop((0, 0, round((ARCS_FROM - MARK_X) * k), mark.height))
dog = resize(dog, (round(dog.width / k), MARK_H))       # 1 px per design unit
small = resize(mark, (26, 24))                           # the top bar's mark, arcs included

out = ["#pragma once\n"
       "// Bitmap fonts and the LapDog mark for the 1.47\" timer's screens (src/lcd147/).\n"
       "// Generated by tools/gen-lcd147-art.py from the app's fonts and lapdog-web's brand\n"
       "// kit - don't edit.\n\n"
       "#include <stdint.h>\n\nnamespace art\n{\n\n"
       "// One character: its bitmap (4-bit alpha, rows packed two pixels a byte, from `off`) sits\n"
       "// x, y from the pen on the baseline (y is up from it, so negative above); advance is in\n"
       "// 1/16 px.\n"
       "struct Glyph\n{\n    uint16_t code;\n    int8_t x, y;\n    uint8_t w, h;\n    uint16_t advance;\n    uint32_t off;\n};\n"
       "struct Font\n{\n    const Glyph *glyphs;\n    uint16_t count;\n    const uint8_t *alpha;\n    int8_t ascent, descent;\n};\n\n"]
total = 0
for spec in SETS:
    f = font_set(*spec)
    total += len(f[4])
    out.append(emit_font(*f) + "\n")
out.append("// The mark: the dog alone (origin at design units MARK_X, MARK_Y; 1 px per unit), and the\n"
           "// whole mark, small. RGB565 plane and 8-bit alpha plane.\n"
           f"constexpr int MARK_X = {MARK_X}, MARK_Y = {MARK_Y};\n")
out.append(image("dog", dog) + "\n" + image("mark", small))
out.append("\n} // namespace art\n")
with open(OUT, "w") as f:
    f.write("".join(out))
print(f"glyph bitmaps {total} bytes, dog {dog.size}, mark {small.size} -> {os.path.normpath(OUT)}")
