#!/usr/bin/env python3
"""Makes the app icon from the game's own title screen (build/icon).

The icon is a frame of the player's copy of the game, so like the
translations it is generated at build time and never committed:

    tools/make_icon.py "<game folder>"            (after building build/mac/vgrun)

It runs the attract mode headless to frame 102, where the title logo is
fully drawn and only "INSERT COINS" shows under it, then lays the scene
(without that line) across a 1024 x 1024 square over a blurred copy of
itself. Outputs, read by CMakeLists.txt when they exist:

    build/icon/icon_1024.png
    build/icon/VoyagerArcade.icns        (Mac app)
    build/icon/Assets.xcassets           (iOS app, compiled by actool)

Needs Pillow; it is installed into build/pylib like Capstone.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PYLIB = os.path.join(ROOT, "build", "pylib")
OUT = os.path.join(ROOT, "build", "icon")
TITLE_FRAME = 102          # the logo is complete; nothing but INSERT COINS below it
LOGO_CENTRE_Y = 410 / 768  # of the frame height
N = 1024


def pillow():
    sys.path.insert(0, PYLIB)
    try:
        import PIL  # noqa: F401
    except ImportError:
        subprocess.check_call([sys.executable, "-m", "pip", "install", "--quiet",
                               "--target", PYLIB, "pillow"])
    from PIL import Image, ImageDraw, ImageFilter
    return Image, ImageDraw, ImageFilter


def title_frame(game, vgrun, work):
    subprocess.check_call([vgrun, "--content", game, "--container", os.path.join(work, "c"),
                           "--frames", str(TITLE_FRAME + 1), "--hash-every", str(TITLE_FRAME),
                           "--frame-dump", os.path.join(work, "f")],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return os.path.join(work, "f_%06d.png" % TITLE_FRAME)


def compose(frame_png, out_png):
    Image, ImageDraw, ImageFilter = pillow()
    src = Image.open(frame_png).convert("RGB")
    w, h = src.size
    # Drop the side margins a little and the INSERT COINS line at the bottom.
    crop = src.crop((round(w * 0.03), 0, round(w * 0.97), round(h * 0.918)))
    k = N / crop.width
    fg = crop.resize((N, round(crop.height * k)), Image.LANCZOS)
    s = max(N / crop.width, N / crop.height)
    bg = crop.resize((round(crop.width * s), round(crop.height * s)), Image.LANCZOS)
    x0, y0 = (bg.width - N) // 2, (bg.height - N) // 2
    bg = bg.crop((x0, y0, x0 + N, y0 + N))
    bg = bg.filter(ImageFilter.GaussianBlur(24)).point(lambda v: int(v * 0.55))
    mask = Image.new("L", fg.size, 255)
    d = ImageDraw.Draw(mask)
    feather = 70
    for i in range(feather):
        a = int(255 * i / feather)
        d.line([(0, i), (N, i)], fill=a)
        d.line([(0, fg.height - 1 - i), (N, fg.height - 1 - i)], fill=a)
    bg.paste(fg, (0, round(N / 2 - LOGO_CENTRE_Y * h * k)), mask)
    bg.save(out_png)
    return Image


def mac_icns(Image, png, icns, work):
    iconset = os.path.join(work, "VoyagerArcade.iconset")
    os.makedirs(iconset, exist_ok=True)
    big = Image.open(png)
    for size in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            name = "icon_%dx%d%s.png" % (size, size, "@2x" if scale == 2 else "")
            big.resize((size * scale, size * scale), Image.LANCZOS).save(os.path.join(iconset, name))
    subprocess.check_call(["iconutil", "-c", "icns", iconset, "-o", icns])


def ios_assets(png, xcassets):
    appicon = os.path.join(xcassets, "AppIcon.appiconset")
    os.makedirs(appicon, exist_ok=True)
    with open(os.path.join(xcassets, "Contents.json"), "w") as f:
        json.dump({"info": {"author": "xcode", "version": 1}}, f, indent=2)
    shutil.copy(png, os.path.join(appicon, "icon_1024.png"))
    with open(os.path.join(appicon, "Contents.json"), "w") as f:
        json.dump({"images": [{"filename": "icon_1024.png", "idiom": "universal",
                               "platform": "ios", "size": "1024x1024"}],
                   "info": {"author": "xcode", "version": 1}}, f, indent=2)


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit("usage: tools/make_icon.py <game folder> [vgrun]")
    game = sys.argv[1]
    vgrun = sys.argv[2] if len(sys.argv) == 3 else os.path.join(ROOT, "build", "mac", "vgrun")
    if not os.path.exists(vgrun):
        sys.exit("build vgrun first (cmake --build --preset mac-host)")
    os.makedirs(OUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as work:
        png = os.path.join(OUT, "icon_1024.png")
        Image = compose(title_frame(game, vgrun, work), png)
        mac_icns(Image, png, os.path.join(OUT, "VoyagerArcade.icns"), work)
    xcassets = os.path.join(OUT, "Assets.xcassets")
    shutil.rmtree(xcassets, ignore_errors=True)
    ios_assets(png, xcassets)
    print("icon written to", OUT)


if __name__ == "__main__":
    main()
