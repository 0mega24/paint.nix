#!/usr/bin/env python3
"""Generate a dark theme from Wine's aero resources.

Usage: win10theme.py <aero-src-dir> <out-dir>
"""
import colorsys
import os
import re
import shutil
import struct
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

def hex2rgb(h):
    h = h.lstrip('#').lower()
    if len(h) == 3:
        h = ''.join(c * 2 for c in h)
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def rgb2hex(c):
    return '#%02x%02x%02x' % tuple(c)


# Explicit mappings for aero colors that need a particular control or glyph role.
SVG_MAP = {
    # Control backgrounds
    'ffffff': '2b2b2b', 'fffffe': '2b2b2b', 'fefefe': '2b2b2b',
    'fffff9': '2b2b2b', 'fdffff': '2b2b2b', 'f5f5f5': '333333',
    # Borders and separators
    'aeaeae': '7a7a7a', '909090': '8c8c8c', 'a6a6a6': '6a6a6a', 'bdbdbd': '5a5a5a',
    # Text and glyphs
    '787878': 'a8a8a8', '5a5a5a': 'c8c8c8', '2d2d2d': 'd6d6d6',
    '282828': 'dadada', '0a0a0a': 'f2f2f2', '000000': 'f2f2f2',
    # Accent and selection colors
    '3096fa': '429ce3',   # hot border
    '2979ff': '0078d7',   # pressed / checked
    '0091ea': '0078d7',
    'e3f2fd': '33414f', 'e1f5fe': '33414f',   # hover tint
    'bbdefb': '264f78', 'b3e5fc': '264f78',   # selection tint
    # Close button
    'ff1744': 'e81123', 'd50000': 'f1707a',
}


def generic_remap(rgb):
    """Invert neutral tones and map blue tones to the accent palette."""
    r, g, b = [x / 255 for x in rgb]
    h, l, s = colorsys.rgb_to_hls(r, g, b)
    if s < 0.2 or l > 0.95 or l < 0.05:
        v = round(43 + (1 - l) * (242 - 43))
        return (v, v, v)
    if 0.5 <= h <= 0.7:
        return hex2rgb('264f78') if l > 0.8 else hex2rgb('0078d7')
    return rgb


def map_color(hexstr):
    key = rgb2hex(hex2rgb(hexstr))[1:]
    return '#' + SVG_MAP.get(key, rgb2hex(generic_remap(hex2rgb(key)))[1:])


COLOR_RE = re.compile(r'((?:fill|stroke|stop-color|color)\s*(?:=\s*"|:\s*))(#[0-9a-fA-F]{6}\b|#[0-9a-fA-F]{3}\b)')
ELEMENT_RE = re.compile(r'<(svg|path|circle|ellipse|rect|g|polygon|line|polyline)\b[^>]*>', re.S)
WHITE = {'#fff', '#ffffff', '#fffffe', '#fefefe'}


def recolor_svg(text, is_glyph):
    def element(m):
        tag, el = m.group(1), m.group(0)
        keep_white = is_glyph and tag in ('path', 'circle', 'ellipse', 'polygon')

        def color(cm):
            if keep_white and cm.group(2).lower() in WHITE:
                return cm.group(1) + '#ffffff'
            return cm.group(1) + map_color(cm.group(2))
        el = COLOR_RE.sub(color, el)
        if tag == 'rect':
            el = re.sub(r'\b(rx|ry)="[^"]*"', r'\1="0"', el)
        return el
    text = ELEMENT_RE.sub(element, text)
    # Shapes without a fill inherit black, which is unreadable on the dark background.
    if not re.search(r'<svg\b[^>]*\sfill=', text):
        text = re.sub(r'<svg\b', '<svg fill="%s"' % map_color('#000000'), text, count=1)
    # Gradient stops are not included in ELEMENT_RE.
    return COLOR_RE.sub(lambda cm: cm.group(1) + map_color(cm.group(2)), text) \
        if 'stop-color' in text else text


def bmp_info(path):
    b = open(path, 'rb').read()
    off, = struct.unpack_from('<I', b, 10)
    w, h, _, bpp = struct.unpack_from('<iiHH', b, 18)
    return b[:off], w, h, bpp


def write_bmp(orig, img, dest, flatten=(43, 43, 43)):
    """Replace pixels while preserving Wine's original BMP header and alpha masks."""
    header, w, h, bpp = bmp_info(orig)
    img = img.convert('RGBA')
    assert img.size == (w, abs(h)), (orig, img.size, (w, h))
    if bpp == 24:
        bg = Image.new('RGBA', img.size, flatten + (255,))
        img = Image.alpha_composite(bg, img)
    stride = (w * bpp + 31) // 32 * 4
    rows = range(abs(h) - 1, -1, -1) if h > 0 else range(abs(h))
    px = img.load()
    out = bytearray(header)
    for y in rows:
        row = bytearray()
        for x in range(w):
            r, g, b, a = px[x, y]
            row += bytes((b, g, r, a)) if bpp == 32 else bytes((b, g, r))
        row += b'\0' * (stride - len(row))
        out += row
    open(dest, 'wb').write(out)


# Caption and border colors are ordered active, inactive.
CAP = [(32, 32, 32), (43, 43, 43)]
BORDER = [(85, 85, 85), (60, 60, 60)]
CLOSE_HOT, CLOSE_PRESSED = (232, 17, 35), (241, 112, 122)
# Each caption has normal, hover, pressed, and disabled glyph colors.
GLYPH = [
    [(255, 255, 255), (255, 255, 255), (255, 255, 255), (93, 93, 93)],
    [(140, 140, 140), (255, 255, 255), (255, 255, 255), (74, 74, 74)],
]


def lighten(c, d):
    return tuple(min(255, v + d) for v in c)


def stack(w, h, n, draw_one):
    """Stack n states in a w-by-h bitmap, calling draw_one for each state."""
    ih = h // n
    strip = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    for i in range(n):
        im = Image.new('RGBA', (w, ih), (0, 0, 0, 0))
        draw_one(im, i)
        strip.paste(im, (0, i * ih))
    return strip


def frame_piece(edges):
    """Draw a frame piece; edges uses l, t, r, and b for its one-pixel borders."""
    def draw(im, i):
        a = i % 2
        d = ImageDraw.Draw(im)
        w, h = im.size
        d.rectangle([0, 0, w - 1, h - 1], fill=CAP[a] + (255,))
        if 'l' in edges:
            d.line([0, 0, 0, h - 1], fill=BORDER[a] + (255,))
        if 'r' in edges:
            d.line([w - 1, 0, w - 1, h - 1], fill=BORDER[a] + (255,))
        if 't' in edges:
            d.line([0, 0, w - 1, 0], fill=BORDER[a] + (255,))
        if 'b' in edges:
            d.line([0, h - 1, w - 1, h - 1], fill=BORDER[a] + (255,))
    return draw


def button_bg(close, sets=2):
    def draw(im, i):
        a, s = (i // 4, i % 4) if sets == 2 else (0, i % 4)
        cap = CAP[a]
        fill = [cap,
                CLOSE_HOT if close else lighten(cap, 26),
                CLOSE_PRESSED if close else lighten(cap, 52),
                cap][s]
        ImageDraw.Draw(im).rectangle([0, 0, im.size[0] - 1, im.size[1] - 1], fill=fill + (255,))
    return draw


def glyph(kind, sets=2):
    def draw(im, i):
        a, s = (i // 4, i % 4) if sets == 2 else (0, i % 4)
        col = GLYPH[a][s]
        n = im.size[0]
        g = max(5, round(n * 0.72))
        sw = max(1, round(n / 13))
        x0 = (n - g) // 2
        y0 = (im.size[1] - g) // 2
        mask = Image.new('L', im.size, 0)
        if kind == 'close':
            S = 8
            big = Image.new('L', (n * S, im.size[1] * S), 0)
            d = ImageDraw.Draw(big)
            p0, p1 = x0 * S, (x0 + g) * S
            q0, q1 = y0 * S, (y0 + g) * S
            d.line([p0, q0, p1, q1], fill=255, width=round(sw * S * 1.05))
            d.line([p0, q1, p1, q0], fill=255, width=round(sw * S * 1.05))
            mask = big.resize(im.size, Image.LANCZOS)
        else:
            d = ImageDraw.Draw(mask)

            def box(x, y, size):
                d.rectangle([x, y, x + size - 1, y + size - 1], outline=255, width=sw)
            if kind == 'max':
                box(x0, y0, g)
            elif kind == 'min':
                yc = y0 + g // 2
                d.rectangle([x0, yc, x0 + g - 1, yc + sw - 1], fill=255)
            elif kind == 'restore':
                o = max(2, round(g * 0.2))
                inner = g - o
                # Only the top and right edges of the rear window are visible.
                d.rectangle([x0 + o, y0, x0 + g - 1, y0 + sw - 1], fill=255)
                d.rectangle([x0 + g - sw, y0, x0 + g - 1, y0 + inner - 1], fill=255)
                box(x0, y0 + o, inner)
            elif kind == 'help':
                try:
                    font = ImageFont.truetype('DejaVuSans.ttf', max(6, round(g * 1.2)))
                except OSError:
                    font = ImageFont.load_default()
                d.text((n / 2, im.size[1] / 2), '?', fill=255, font=font, anchor='mm')
        solid = Image.new('RGBA', im.size, col + (255,))
        im.paste(solid, (0, 0), mask)
    return draw


def window_part(name, w, h):
    """Draw a window bitmap, or return None to use its recolored SVG."""
    base = name[len('blue_window_'):-4]
    if base in ('caption', 'caption_sizing_template', 'small_caption', 'small_caption_sizing_template'):
        return stack(w, h, 2, frame_piece('ltr'))
    if base == 'min_caption':
        return stack(w, h, 2, frame_piece('ltrb'))
    if base == 'max_caption':
        return stack(w, h, 2, frame_piece(''))
    if base in ('frame_left', 'small_frame_left'):
        return stack(w, h, 2, frame_piece('l'))
    if base in ('frame_right', 'small_frame_right'):
        return stack(w, h, 2, frame_piece('r'))
    if base in ('frame_bottom', 'small_frame_bottom'):
        return stack(w, h, 2, frame_piece('lrb'))
    m = re.match(r'(mdi_)?(close|min|max|restore|help|small_close)_button_(background|glyph)', base)
    if m:
        mdi, kind, part = m.groups()
        n = 4 if mdi else 8
        sets = 1 if mdi else 2
        if part == 'background':
            return stack(w, h, n, button_bg(kind in ('close', 'small_close'), sets))
        return stack(w, h, n, glyph('close' if kind == 'small_close' else kind, sets))
    return None


SYSCOLORS = {
    'Scrollbar': '23 23 23', 'Background': '0 0 0',
    'ActiveCaption': '32 32 32', 'InactiveCaption': '43 43 43',
    'Menu': '43 43 43', 'Window': '32 32 32', 'WindowFrame': '85 85 85',
    'MenuText': '255 255 255', 'WindowText': '255 255 255', 'CaptionText': '255 255 255',
    'ActiveBorder': '85 85 85', 'InactiveBorder': '60 60 60', 'AppWorkSpace': '32 32 32',
    'Highlight': '0 120 215', 'HighlightText': '255 255 255',
    'BtnFace': '51 51 51', 'BtnShadow': '30 30 30', 'GrayText': '128 128 128',
    'BtnText': '255 255 255', 'InactiveCaptionText': '140 140 140',
    'BtnHighlight': '77 77 77', 'DkShadow3d': '15 15 15', 'Light3d': '64 64 64',
    'InfoText': '255 255 255', 'InfoBk': '43 43 43', 'ButtonAlternateFace': '51 51 51',
    'HotTracking': '66 156 227', 'GradientActiveCaption': '32 32 32',
    'GradientInactiveCaption': '43 43 43', 'MenuHilight': '65 65 65', 'MenuBar': '43 43 43',
}
COLOR_KEY_RE = re.compile(r'^"(\w*Color\w*|Edge\w+) = (\d+) (\d+) (\d+)\\r\\n"$')


def patch_rc(text):
    out, in_blue, in_sys = [], False, False
    for line in text.split('\n'):
        if line.startswith('BLUE_INI TEXTFILE'):
            in_blue = True
        elif in_blue and line.startswith('}'):
            in_blue = False
        if in_blue:
            sec = re.match(r'^"(?:\\r\\n)?\[([^\]]+)\]', line)
            if sec:
                in_sys = sec.group(1) == 'SysMetrics'
            m = re.match(r'^"(\w+) = (\d+ \d+ \d+)\\r\\n"$', line)
            if in_sys and m and m.group(1) in SYSCOLORS:
                line = '"%s = %s\\r\\n"' % (m.group(1), SYSCOLORS[m.group(1)])
            m = COLOR_KEY_RE.match(line)
            if m and not in_sys and m.group(1) != 'TransparentColor':
                c = map_color(rgb2hex(tuple(int(x) for x in m.groups()[1:])))
                line = '"%s = %d %d %d\\r\\n"' % ((m.group(1),) + hex2rgb(c))
            if re.match(r'^"(\w*Font) = ', line):
                line = re.sub(r'= [^,]+, (\d+)(, bold)?', '= Segoe UI, 9', line)
        line = line.replace('"DisplayName = Aero\\r\\n"', '"DisplayName = Windows 10 Dark\\r\\n"')
        line = line.replace('"ToolTip = Aero Visual Style\\r\\n"', '"ToolTip = Windows 10 style (dark) for Wine\\r\\n"')
        out.append(line)
    return widen_data_strings('\n'.join(out))


def widen_data_strings(text):
    """Use wide literals so standalone wrc emits the UTF-16 uxtheme expects."""
    out, in_data = [], False
    for line in text.split('\n'):
        if re.match(r'^\w+ (TEXTFILE|COLORNAMES|SIZENAMES|FILERESNAMES)\b', line):
            in_data = True
        elif in_data and line.startswith('}'):
            in_data = False
        elif in_data:
            line = re.sub(r'^(\s*)"', r'\1L"', line)
        out.append(line)
    return '\n'.join(out)


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: win10theme.py <aero-src-dir> <out-dir>')
    src, out = sys.argv[1:]
    if os.path.commonpath([os.path.realpath(src), os.path.realpath(out)]) == os.path.realpath(out):
        sys.exit('output directory must not contain the source directory')
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, 'svg'))
    for f in os.listdir(src):
        if f.endswith(('.bmp', '.h')):
            shutil.copy(os.path.join(src, f), out)
    rc = open(os.path.join(src, 'aero.rc'), encoding='utf-8').read()
    open(os.path.join(out, 'win10dark.rc'), 'w', encoding='utf-8').write(patch_rc(rc))

    to_render = []
    drawn = 0
    for f in sorted(os.listdir(src)):
        if not f.endswith('.bmp'):
            continue
        orig = os.path.join(src, f)
        _, w, h, _ = bmp_info(orig)
        if f.startswith('blue_window_'):
            img = window_part(f, w, abs(h))
            if img is not None:
                write_bmp(orig, img, os.path.join(out, f), flatten=CAP[0])
                drawn += 1
                continue
        svg = os.path.join(src, f[:-4] + '.svg')
        text = open(svg, encoding='utf-8').read()
        dst = os.path.join(out, 'svg', f[:-4] + '.svg')
        open(dst, 'w', encoding='utf-8').write(recolor_svg(text, 'glyph' in f or 'arrow' in f))
        to_render.append(dst)

    # Limit argv length while sharing Inkscape startup across multiple images.
    for i in range(0, len(to_render), 120):
        subprocess.run(['inkscape', '--export-type=png', '--export-overwrite'] + to_render[i:i + 120],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for svg in to_render:
        name = os.path.basename(svg)[:-4]
        write_bmp(os.path.join(src, name + '.bmp'), Image.open(svg[:-4] + '.png'),
                  os.path.join(out, name + '.bmp'))
    print('drawn %d window parts, recolored %d control bitmaps' % (drawn, len(to_render)))


if __name__ == '__main__':
    main()
