#!/usr/bin/env python3
"""Generate Chamows fonts and icons.

Writes:
  <out>/assets.h          kernel fonts + system icons (C arrays)
  <out>/icons/<name>.rgba  24, 32 and 48 px RGBA icons for app packages

Fonts are rendered with Pillow from fonts installed on the build machine
(Inter for the interface, DejaVu Sans Mono for the terminal), Latin-1 range,
8-bit antialiasing.
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "build"

FONT_DIRS = ["/usr/share/fonts", "/usr/local/share/fonts", os.path.expanduser("~/.fonts"),
             "/Library/Fonts", "/System/Library/Fonts"]


def find_font(*names):
    for d in FONT_DIRS:
        for root, _, files in os.walk(d):
            for n in names:
                if n in files:
                    return os.path.join(root, n)
    sys.exit(f"genassets: no encuentro ninguna de estas fuentes: {names}")


def render_font(path, size, cname):
    f = ImageFont.truetype(path, size)
    ascent, descent = f.getmetrics()
    height = ascent + descent
    glyphs, data = [], bytearray()
    for code in range(32, 256):
        ch = bytes([code]).decode("latin-1")
        if 127 <= code < 160:
            ch = " "
        adv = max(1, round(f.getlength(ch)))
        l, _, r, _ = f.getbbox(ch, anchor="ls")
        x0 = min(0, l)
        w = max(adv, r) - x0 + 1
        img = Image.new("L", (w, height), 0)
        ImageDraw.Draw(img).text((-x0, ascent), ch, font=f, fill=255, anchor="ls")
        glyphs.append((len(data), w, adv, x0))
        data += img.tobytes()
    lines = [f"static const u8 {cname}_data[{len(data)}] = {{"]
    for i in range(0, len(data), 32):
        lines.append("  " + ",".join(str(b) for b in data[i:i + 32]) + ",")
    lines.append("};")
    lines.append(f"static const glyph_t {cname}_glyphs[224] = {{")
    lines += [f"  {{{o},{w},{a},{x}}}," for o, w, a, x in glyphs]
    lines.append("};")
    lines.append(f"static const font_t {cname} = {{ {height}, {ascent}, {cname}_glyphs, {cname}_data }};")
    return "\n".join(lines)


# ---------------------------------------------------------------- icons
S = 4  # supersampling


def tile(size, c1, c2, radius=0.24):
    big = size * S
    grad = Image.new("RGBA", (big, big))
    gd = ImageDraw.Draw(grad)
    a, b = Image.new("RGB", (1, 1), c1).getpixel((0, 0)), Image.new("RGB", (1, 1), c2).getpixel((0, 0))
    for y in range(big):
        t = y / (big - 1)
        gd.line([(0, y), (big, y)], fill=tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3)) + (255,))
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle([S, S, big - S - 1, big - S - 1], radius=int(big * radius), fill=255)
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)
    return img


def finish(img, size):
    return img.resize((size, size), Image.LANCZOS)


def P(v, big):  # fraction -> pixels
    return v * big


def draw_icon(name, size):
    W = (255, 255, 255, 255)
    defs = {
        "logo": ("#2aa7ff", "#5b3cf0"), "store": ("#3aa0ff", "#5b3cf0"), "terminal": ("#3b3f4c", "#14161c"),
        "pc": ("#4fb3ff", "#1f6fe5"), "app": ("#9aa3b8", "#5c6478"), "calc": ("#5a6274", "#2b303c"),
        "notepad": ("#4fb3ff", "#1f6fe5"), "paint": ("#ff7aa8", "#c2348a"), "clock": ("#7c8cff", "#3f4fd8"),
        "snake": ("#a6e34f", "#3f9c2f"), "folder": ("#ffd25c", "#e8a200"),
    }
    c1, c2 = defs[name]
    img = tile(size, c1, c2)
    big = size * S
    d = ImageDraw.Draw(img)
    p = lambda v: P(v, big)
    lw = max(S, int(big * 0.085))
    if name == "logo":
        d.arc([p(.24), p(.24), p(.76), p(.76)], 40, 320, fill=W, width=int(big * .12))
        d.ellipse([p(.66), p(.44), p(.78), p(.56)], fill=W)
    elif name == "store":
        d.polygon([(p(.22), p(.38)), (p(.78), p(.38)), (p(.73), p(.82)), (p(.27), p(.82))], fill=W)
        d.arc([p(.36), p(.2), p(.64), p(.52)], 180, 360, fill=W, width=lw)
        d.arc([p(.38), p(.48), p(.62), p(.72)], 40, 320, fill=c2, width=lw)
    elif name == "terminal":
        d.line([(p(.26), p(.36)), (p(.42), p(.5)), (p(.26), p(.64))], fill=W, width=lw, joint="curve")
        d.line([(p(.5), p(.66)), (p(.74), p(.66))], fill=W, width=lw)
    elif name == "pc":
        d.rounded_rectangle([p(.2), p(.24), p(.8), p(.64)], radius=p(.04), fill=W)
        d.rectangle([p(.45), p(.64), p(.55), p(.74)], fill=W)
        d.rounded_rectangle([p(.32), p(.73), p(.68), p(.79)], radius=p(.03), fill=W)
    elif name == "app":
        for i, (x, y) in enumerate([(.24, .24), (.54, .24), (.24, .54), (.54, .54)]):
            d.rounded_rectangle([p(x), p(y), p(x + .22), p(y + .22)], radius=p(.05), fill=W)
    elif name == "calc":
        d.rounded_rectangle([p(.26), p(.18), p(.74), p(.82)], radius=p(.06), fill=W)
        d.rectangle([p(.33), p(.25), p(.67), p(.38)], fill=c2)
        for yy in (.48, .6, .72):
            for xx in (.36, .5, .64):
                d.ellipse([p(xx - .04), p(yy - .04), p(xx + .04), p(yy + .04)], fill=c2)
    elif name == "notepad":
        d.rounded_rectangle([p(.26), p(.18), p(.74), p(.82)], radius=p(.05), fill=W)
        for yy in (.36, .5, .64):
            d.line([(p(.34), p(yy)), (p(.66 if yy < .6 else .54), p(yy))], fill=c2, width=max(S, lw // 2))
    elif name == "paint":
        d.polygon([(p(.58), p(.18)), (p(.82), p(.42)), (p(.48), p(.76)), (p(.24), p(.52))], fill=W)
        d.ellipse([p(.16), p(.6), p(.4), p(.84)], fill=(255, 220, 90, 255))
    elif name == "clock":
        d.ellipse([p(.18), p(.18), p(.82), p(.82)], fill=W)
        d.line([(p(.5), p(.5)), (p(.5), p(.3))], fill=c2, width=lw)
        d.line([(p(.5), p(.5)), (p(.65), p(.58))], fill=c2, width=lw)
    elif name == "snake":
        d.line([(p(.2), p(.7)), (p(.36), p(.7)), (p(.36), p(.44)), (p(.62), p(.44)), (p(.62), p(.3))],
               fill=W, width=int(big * .13), joint="curve")
        d.ellipse([p(.66), p(.62), p(.8), p(.76)], fill=(255, 90, 79, 255))
    elif name == "folder":
        d.rounded_rectangle([p(.18), p(.28), p(.82), p(.76)], radius=p(.05), fill=W)
    return finish(img, size)


def rgba_c(name, img):
    px = list(img.getdata())
    vals = [f"0x{a:02x}{r:02x}{g:02x}{b:02x}" for r, g, b, a in px]
    rows = [",".join(vals[i:i + 12]) for i in range(0, len(vals), 12)]
    return f"static const u32 {name}[{len(vals)}] = {{\n  " + ",\n  ".join(rows) + "\n};"


def main():
    os.makedirs(os.path.join(OUT, "icons"), exist_ok=True)
    ui = find_font("Inter-Regular.otf", "Inter-Regular.ttf", "DejaVuSans.ttf")
    ui_bold = find_font("Inter-SemiBold.otf", "Inter-SemiBold.ttf", "DejaVuSans-Bold.ttf")
    mono = find_font("DejaVuSansMono.ttf")
    parts = [
        "/* Generated by tools/genassets.py. Do not edit. */",
        "#ifndef CHAMOWS_ASSETS_H", "#define CHAMOWS_ASSETS_H", '#include "gfx.h"',
        render_font(ui, 13, "font_ui"),
        render_font(ui_bold, 13, "font_bold"),
        render_font(ui_bold, 22, "font_title"),
        render_font(mono, 13, "font_mono"),
    ]
    for name in ["logo", "store", "terminal", "pc", "app", "folder"]:
        for size in (24, 32, 48):
            parts.append(rgba_c(f"icon_{name}_{size}", draw_icon(name, size)))
    parts.append("#endif")
    with open(os.path.join(OUT, "assets.h"), "w") as fh:
        fh.write("\n".join(parts) + "\n")
    for name in ["calc", "notepad", "paint", "clock", "snake"]:
        raw = bytearray()
        for size in (24, 32, 48):  # all three sizes, one after another
            for r, g, b, a in draw_icon(name, size).getdata():
                raw += bytes([b, g, r, a])  # little-endian 0xAARRGGBB
        with open(os.path.join(OUT, "icons", name + ".rgba"), "wb") as fh:
            fh.write(raw)


if __name__ == "__main__":
    main()
