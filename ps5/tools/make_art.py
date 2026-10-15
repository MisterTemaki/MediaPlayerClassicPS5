#!/usr/bin/env python3
# Media Player Classic PS5: the home screen art from MPC-HC's own pictures (until there is art of its own):
#   make_art.py <sce_sys dir> <bc7enc>
#   icon0.png        512x512: MPC-HC's icon (res/icon.ico, 256 px) on a dark square
#   background-source.png, pic0.dds, pic1.dds: MPC-HC's logo (res/logo.3.png) on black, 3840x2160 BC7 DX10 DDS
#                    without mipmaps (bc7enc_rdo), with the header the home screen takes
import os, shutil, struct, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFilter

out, bc7enc = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
res = os.path.join(here, '..', 'assets', 'res')

def gradient(w, h, top, bottom):
    im = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return im

icon = Image.open(os.path.join(res, 'icon-256.png')).convert('RGBA')
ic = gradient(512, 512, (40, 44, 52), (8, 8, 10)).convert('RGBA')
big = icon.resize((400, 400), Image.LANCZOS)
shadow = Image.new('RGBA', (512, 512), (0, 0, 0, 0))
shadow.paste((0, 0, 0, 160), (56, 66), big)
shadow = shadow.filter(ImageFilter.GaussianBlur(10))
ic = Image.alpha_composite(ic, shadow)
ic.alpha_composite(big, (56, 52))
ic.convert('RGB').save(os.path.join(out, 'icon0.png'))

logo = Image.open(os.path.join(res, 'logo.3.png')).convert('RGB')
bg = gradient(3840, 2160, (14, 16, 20), (0, 0, 0))
lw = 1900
lg = logo.resize((lw, int(logo.height * lw / logo.width)), Image.LANCZOS)
bg.paste(lg, ((3840 - lw) // 2, (2160 - lg.height) // 2 - 80))
bg.resize((1920, 1080), Image.LANCZOS).save(os.path.join(out, 'background-source.png'))
if bc7enc:
    tmp = tempfile.mkdtemp()
    src = os.path.join(tmp, 'bg.png')
    bg.save(src)
    subprocess.run([bc7enc, '-q', '-g', src, os.path.join(tmp, 'bg.dds')], check=True, cwd=tmp)
    dds = bytearray(open(os.path.join(tmp, 'bg.dds'), 'rb').read())
    assert dds[:4] == b'DDS ' and dds[84:88] == b'DX10'
    flags = struct.unpack_from('<I', dds, 8)[0] | 0x20000
    struct.pack_into('<I', dds, 8, flags)
    struct.pack_into('<I', dds, 24, 1)
    struct.pack_into('<I', dds, 28, 1)
    struct.pack_into('<I', dds, 144, 1)
    for name in ('pic0.dds', 'pic1.dds'):
        open(os.path.join(out, name), 'wb').write(dds)
    shutil.rmtree(tmp)
print('art written to', out)
