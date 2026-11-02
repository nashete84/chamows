#!/usr/bin/env python3
"""Make CHAMOWSI.TTF: Chamows's own icon font, made of simple strokes.

Qt's Windows 11 style draws check marks, arrows and title-bar buttons with
characters from an icon font it looks up as "Segoe Fluent Icons". That font
is not free, so Chamows ships this one under that family name, with the same
code points (the ones Qt uses). Usage: make-iconfont.py OUT.TTF"""
import math, sys
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

UPM, CX, CY = 1000, 500, 300  # glyph box centre: half way between ascent 800 and descent -200

def poly(pen, pts):
    area = sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]))
    if area > 0: pts = pts[::-1]  # TrueType outer contours go clockwise
    pen.moveTo(pts[0])
    for p in pts[1:]: pen.lineTo(p)
    pen.closePath()

def line(pen, x0, y0, x1, y1, w=70):
    """a stroke between two points, given around the centre (-400..400)"""
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy) or 1
    ox, oy = -dy / n * w / 2, dx / n * w / 2
    ex, ey = dx / n * w / 2, dy / n * w / 2  # square caps, so joined strokes meet cleanly
    p = [(x0 - ex + ox, y0 - ey + oy), (x1 + ex + ox, y1 + ey + oy), (x1 + ex - ox, y1 + ey - oy), (x0 - ex - ox, y0 - ey - oy)]
    poly(pen, [(round(CX + x), round(CY + y)) for x, y in p])

def tri(pen, pts):
    poly(pen, [(round(CX + x), round(CY + y)) for x, y in pts])

def box(pen, x0, y0, x1, y1, w=60):
    line(pen, x0, y0, x1, y0, w); line(pen, x1, y0, x1, y1, w)
    line(pen, x1, y1, x0, y1, w); line(pen, x0, y1, x0, y0, w)

def chevron(pen, d, s=1.0):  # d: 'down', 'up', 'left', 'right'
    a, b = 300 * s, 150 * s
    pts = {'down': [(-a, b), (0, -b), (a, b)], 'up': [(-a, -b), (0, b), (a, -b)],
           'left': [(b, a), (-b, 0), (b, -a)], 'right': [(-b, a), (b, 0), (-b, -a)]}[d]
    line(pen, *pts[0], *pts[1]); line(pen, *pts[1], *pts[2])

def caret(pen, d):
    pts = {'down': [(-250, 120), (250, 120), (0, -160)], 'up': [(-250, -120), (0, 160), (250, -120)],
           'left': [(120, -250), (120, 250), (-160, 0)], 'right': [(-120, 250), (-120, -250), (160, 0)]}[d]
    tri(pen, pts)

def dot(pen, x, y, r=55):
    tri(pen, [(x + r * math.cos(a * math.pi / 8), y + r * math.sin(a * math.pi / 8)) for a in range(16)])

ICONS = {
    0xE629: lambda p: line(p, -300, 0, 300, 0),                                   # Dash12
    0xE73E: lambda p: (line(p, -320, 0, -100, -230), line(p, -100, -230, 330, 260)),  # CheckMark
    0xF78C: lambda p: (line(p, -420, 20, -140, -280, 110), line(p, -140, -280, 440, 360, 110)),  # AcceptMedium (check box)
    0xEDD9: lambda p: caret(p, 'left'), 0xEDDA: lambda p: caret(p, 'right'),
    0xEDDB: lambda p: caret(p, 'up'), 0xEDDC: lambda p: caret(p, 'down'),
    0xE70D: lambda p: chevron(p, 'down'), 0xE70E: lambda p: chevron(p, 'up'),
    0xE971: lambda p: chevron(p, 'up', .8), 0xE972: lambda p: chevron(p, 'down', .8),
    0xE973: lambda p: chevron(p, 'left', .8), 0xE974: lambda p: chevron(p, 'right', .8),
    0xE96D: lambda p: chevron(p, 'up', .6), 0xE96E: lambda p: chevron(p, 'down', .6),
    0xE921: lambda p: line(p, -300, 0, 300, 0, 40),                                # ChromeMinimize
    0xE922: lambda p: box(p, -280, -280, 280, 280, 40),                            # ChromeMaximize
    0xE923: lambda p: (box(p, -280, -280, 180, 180, 40), line(p, -180, 280, 280, 280, 40),
                       line(p, 280, 280, 280, -180, 40)),                         # ChromeRestore
    0xE8BB: lambda p: (line(p, -280, -280, 280, 280, 40), line(p, -280, 280, 280, -280, 40)),  # ChromeClose
    0xE894: lambda p: (line(p, -250, -250, 250, 250), line(p, -250, 250, 250, -250)),          # Clear
    0xE712: lambda p: (dot(p, -260, 0), dot(p, 0, 0), dot(p, 260, 0)),             # More
    0xE897: lambda p: (line(p, -170, 200, -50, 290), line(p, -50, 290, 120, 270), line(p, 120, 270, 180, 160),
                       line(p, 180, 160, 0, 30), line(p, 0, 30, 0, -100), dot(p, 0, -260, 50)),  # Help
}

def main(out):
    names = ['.notdef'] + ['u%04X' % c for c in sorted(ICONS)]
    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({c: 'u%04X' % c for c in ICONS})
    glyphs = {'.notdef': TTGlyphPen(None).glyph()}
    for c, draw in ICONS.items():
        pen = TTGlyphPen(None); draw(pen); glyphs['u%04X' % c] = pen.glyph()
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({n: (UPM, 0) for n in names})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fam = 'Segoe Fluent Icons'  # the name Qt asks for; the drawings are Chamows's own
    fb.setupNameTable({'familyName': fam, 'styleName': 'Regular', 'uniqueFontIdentifier': 'Chamows Icons',
                       'fullName': fam, 'psName': 'ChamowsIcons', 'version': 'Version 1.0',
                       'copyright': 'Chamows project, free to use'})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200,
                ulUnicodeRange1=0, ulCodePageRange1=1)
    fb.setupPost()
    fb.save(out)

main(sys.argv[1])
