"""make_icons.py SRC NAME OUTDIR: the tile's icon.png (512x423, the TV's app
icon size) and logo.png (256x256, shown while opening). SRC: a PNG/SVG file or
URL, or empty for a letter tile."""
import io, subprocess, sys, urllib.request
from PIL import Image, ImageDraw, ImageFont

src, name, out = sys.argv[1], sys.argv[2], sys.argv[3]
BG = (20, 24, 33)

def load(src):
    if not src:
        return None
    data = urllib.request.urlopen(src, timeout=10).read() if src.startswith(("http://", "https://")) else open(src, "rb").read()
    if src.lower().endswith(".svg") or data.lstrip().startswith(b"<"):
        data = subprocess.run(["rsvg-convert", "-w", "512", "-h", "512"], input=data, capture_output=True, check=True).stdout
    return Image.open(io.BytesIO(data)).convert("RGBA")

def letter(size):
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, size - 1, size - 1), radius=size // 5, fill=(45, 108, 223, 255))
    try:
        font = ImageFont.truetype("DejaVuSans-Bold.ttf", int(size * 0.6))
    except OSError:
        font = ImageFont.load_default()
    d.text((size / 2, size / 2), name[:1].upper(), font=font, fill="white", anchor="mm")
    return im

logo = load(src) or letter(512)
logo.thumbnail((256, 256), Image.LANCZOS)
logo.save(f"{out}/logo.png")
big = (load(src) or letter(512)).copy()

def dark(im):  # mean brightness of the logo's visible pixels
    px = [(r, g, b) for r, g, b, a in im.resize((64, 64)).getdata() if a > 128]
    return bool(px) and sum(0.3 * r + 0.59 * g + 0.11 * b for r, g, b in px) / len(px) < 70

icon = Image.new("RGBA", (512, 423), ((232, 236, 242) if dark(big) else BG) + (255,))
big.thumbnail((260, 260), Image.LANCZOS)
icon.alpha_composite(big, ((512 - big.width) // 2, (423 - big.height) // 2))
icon.convert("RGB").save(f"{out}/icon.png")
