"""Rebuild the original Psychic lettering and orbital eye. Run with Python and Pillow."""
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

WIDTH, HEIGHT, SCALE = 219, 39, 4
SIZE = (WIDTH * SCALE, HEIGHT * SCALE)
art = Image.new("RGB", SIZE, "#0c0b1b")
ink = ImageDraw.Draw(art)


def points(coords):
    return [(round(x * SCALE), round(y * SCALE)) for x, y in coords]


def stroke(coords, color, width=1):
    ink.line(points(coords), fill=color, width=max(1, round(width * SCALE)), joint="curve")


def orbit(x, y, a, b, tilt, color, width=.8, start=0, end=2 * math.pi):
    route = []
    for i in range(121):
        angle = start + (end - start) * i / 120
        u, v = a * math.cos(angle), b * math.sin(angle)
        route.append((x + u * math.cos(tilt) - v * math.sin(tilt),
                      y + u * math.sin(tilt) + v * math.cos(tilt)))
    stroke(route, color, width)


# Slanted custom capitals. The P counter is punched through the face.
glyphs = {
    'P': (22, [(0,0),(17,0),(22,4),(22,11),(17,15),(7,15),(7,21),(0,21)],
          [(7,5),(15,5),(16,6),(16,9),(14,10),(7,10)]),
    'S': (23, [(5,0),(23,0),(21,5),(8,5),(6,7),(7,8),(18,8),(22,12),(21,17),
               (17,21),(0,21),(2,16),(14,16),(16,14),(15,13),(5,13),(0,9),(1,4)]),
    'Y': (24, [(0,0),(8,0),(12,8),(17,0),(25,0),(15,14),(15,21),(8,21),(8,14)]),
    'C': (22, [(6,0),(22,0),(20,5),(9,5),(7,7),(7,14),(9,16),(22,16),
               (19,21),(5,21),(0,16),(0,5)]),
    'H': (23, [(0,0),(7,0),(7,8),(15,8),(15,0),(22,0),(22,21),(15,21),(15,13),
               (7,13),(7,21),(0,21)]),
    'I': (8, [(0,0),(7,0),(7,21),(0,21)]),
}
mask = Image.new("L", SIZE)
pen = ImageDraw.Draw(mask)
x = 3
for letter in "PSYCHIC":
    width, face, *counters = glyphs[letter]
    for index, path in enumerate([face, *counters]):
        outline = [(x + u + (21-v) * .33, 8 + v) for u, v in path]
        pen.polygon(points(outline), 0 if index else 255)
    x += width + 1.3

# Two offset inks and a narrow specular edge frame the chrome reflection.
for dx, dy, color in [(1.7, 2.4, "#603cb0"), (-.7, .8, "#5cb9f3")]:
    art.paste(color, (0, 0, *SIZE), ImageChops.offset(mask, round(dx*SCALE), round(dy*SCALE)))
bands = [(0, "#ffffff"), (.13, "#f3f4ff"), (.35, "#9da8ec"), (.47, "#7086c8"),
         (.49, "#25265d"), (.59, "#39315e"), (.75, "#a79bfa"), (.89, "#efecff"), (1, "#8094d9")]
bands = [(edge, bytes.fromhex(rgb[1:])) for edge, rgb in bands]
chrome = Image.new("RGB", SIZE)
pixels = chrome.load()
for y in range(8*SCALE, 30*SCALE):
    for x in range(166*SCALE):
        depth = min(1., max(0., (y/SCALE - 8 + 1.45*math.sin(x/SCALE/24)) / 21))
        for (lo, c), (hi, d) in zip(bands, bands[1:]):
            if depth <= hi:
                t = (depth-lo)/(hi-lo)
                pixels[x,y] = tuple(round(a+(b-a)*t) for a,b in zip(c,d))
                break
art.paste(chrome, mask=mask)
edge = ImageChops.subtract(mask, mask.filter(ImageFilter.MinFilter(7)))
art.paste("#c1c9fc", (0,0,*SIZE), edge)

# An eye carrying a tilted planetary ring. Draw its far arc behind the eyelid.
cx, cy = 188, 19
orbit(cx,cy,28,6,-.39,"#7867d5",1)
almond = [(165,19),(170,15),(176,10),(183,7),(191,7),(198,9),(205,14),
          (211,17),(205,20),(199,25),(191,29),(182,30),(174,27)]
ink.polygon(points(almond), "#271741")
stroke(almond + [almond[0]], "#c6bcfb", 1.2)
stroke([(166,19),(175,14),(183,10),(192,9),(201,12),(211,17)], "#ebefff", 1.5)
stroke([(165,19),(174,24),(182,27),(193,25),(203,20),(211,17)], "#8782d4", 1)
orbit(cx,cy-1,9,9,-.25,"#eaff67",2)
orbit(cx,cy-1,6.6,7,-.25,"#8f94d4",1)
orbit(cx,cy-1,3,5,-.25,"#defb49",2.2)
orbit(cx,cy,28,6,-.39,"#171024",2.6,start=0,end=math.pi)
orbit(cx,cy,28,6,-.39,"#dcfa59",1.2,start=0,end=math.pi)
for x,y,radius in [(12,7.5,3.3),(151,26,2.3),(207,7,3.6)]:
    ink.polygon(points([(x,y-radius),(x+.55,y-.6),(x+radius,y),(x+.55,y+.6),
                        (x,y+radius),(x-.55,y+.6),(x-radius,y),(x-.55,y-.6)]), "#ffffff")

# Indexed pixels keep the host paint path to one small blit, including on an A55.
indexed = art.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS).quantize(64)
palette = bytes(indexed.getpalette())[:64*3]
values = ['0xff'+palette[i:i+3].hex() for i in range(0,len(palette),3)]
raster = list(indexed.tobytes())


def rows(items, count):
    return ''.join('    '+', '.join(map(str, items[i:i+count]))+',\n' for i in range(0,len(items),count))


target = Path(__file__).resolve().parents[1] / 'src/ui/masthead.h'
target.write_text('// Original artwork. Regenerate with tools/draw-masthead.py.\n#pragma once\n#include <cstdint>\n\n'
                  'namespace psychic::masthead\n{\n'
                  f'inline constexpr int width = {WIDTH}, height = {HEIGHT};\n'
                  'inline constexpr uint32_t palette[] = {\n'+rows(values,8)+'};\n'
                  'inline constexpr uint8_t pixels[width * height] = {\n'+rows(raster,32)+'};\n}\n')
