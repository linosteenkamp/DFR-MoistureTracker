#!/usr/bin/env python3
"""Generate include/display_assets.h with bitmap fonts, icons, and QR code.

One-shot script. Run manually when the URL or fonts change. Output is
committed so the build doesn't depend on Python at compile time.

Dependencies: qrcode, Pillow.
    python3 -m pip install --user qrcode Pillow
"""
import os
import platform
import sys

try:
    import qrcode
    from PIL import Image, ImageDraw, ImageFont
except ImportError as e:
    sys.exit(f"missing dependency: {e}. Run: pip install --user qrcode Pillow")

OUT_PATH = "include/display_assets.h"
PORTAL_URL = "http://192.168.4.1"

# QR rendering: version-2 + ECC-L = 25x25 modules. Scaled 3x = 75x75 px.
QR_VERSION = 2
QR_SCALE = 3

# Small font: 6x8 pixels, ASCII printable (32-126), rendered from a system mono font.
SMALL_W, SMALL_H = 6, 8
SMALL_PT = 8

# Large font: 24x32 pixels, digits + period + percent only.
LARGE_W, LARGE_H = 24, 32
LARGE_CHARS = "0123456789.%"
LARGE_PT = 32


def find_mono_font():
    """Return path to a usable monospace TTF on this system."""
    candidates = []
    sysname = platform.system()
    if sysname == "Darwin":
        candidates += [
            "/System/Library/Fonts/Menlo.ttc",
            "/System/Library/Fonts/Monaco.dfont",
            "/Library/Fonts/Courier New.ttf",
        ]
    candidates += [
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise FileNotFoundError(
        "no monospace TTF found. Set MONO_FONT env var to an existing TTF path."
    )


def matrix_to_bytes_msb(matrix, w, h):
    """Pack a 2D bool matrix into row-major bytes, MSB-first within byte."""
    bpr = (w + 7) // 8
    out = bytearray()
    for y in range(h):
        for bx in range(bpr):
            b = 0
            for bit in range(8):
                x = bx * 8 + bit
                if x < w and matrix[y][x]:
                    b |= 1 << (7 - bit)
            out.append(b)
    return bytes(out), bpr


def render_qr():
    qr = qrcode.QRCode(
        version=QR_VERSION,
        error_correction=qrcode.constants.ERROR_CORRECT_L,
        box_size=1,
        border=0,
    )
    qr.add_data(PORTAL_URL)
    qr.make(fit=False)
    matrix = qr.get_matrix()
    base_w = len(matrix[0])
    base_h = len(matrix)
    w = base_w * QR_SCALE
    h = base_h * QR_SCALE
    scaled = [[matrix[y // QR_SCALE][x // QR_SCALE] for x in range(w)] for y in range(h)]
    data, bpr = matrix_to_bytes_msb(scaled, w, h)
    return data, w, h, bpr


def render_glyph(font, ch, w, h):
    """Render one character to a 1-bit (bool) matrix of size w x h.
    Black pixels (where text was drawn) = True."""
    img = Image.new("1", (w, h), 1)  # 1 = white background
    draw = ImageDraw.Draw(img)
    draw.text((0, 0), ch, font=font, fill=0)  # 0 = black foreground
    return [[img.getpixel((x, y)) == 0 for x in range(w)] for y in range(h)]


def render_font(chars, font_path, pt, w, h):
    """Render each character and concatenate the byte arrays."""
    font = ImageFont.truetype(font_path, pt)
    out = bytearray()
    for ch in chars:
        m = render_glyph(font, ch, w, h)
        data, _ = matrix_to_bytes_msb(m, w, h)
        out.extend(data)
    return bytes(out)


def emit_array(name, data, comment=""):
    s = ""
    if comment:
        s += f"// {comment}\n"
    s += f"static const uint8_t {name}[{len(data)}] = {{\n"
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        s += "    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n"
    s += "};\n\n"
    return s


def main():
    font_path = os.environ.get("MONO_FONT") or find_mono_font()
    print(f"using font: {font_path}", file=sys.stderr)

    qr_data, qr_w, qr_h, qr_bpr = render_qr()
    print(f"QR: {qr_w}x{qr_h} px, {len(qr_data)} bytes", file=sys.stderr)

    small_chars = "".join(chr(i) for i in range(32, 127))
    small_data = render_font(small_chars, font_path, SMALL_PT, SMALL_W, SMALL_H)
    print(f"small font: {len(small_chars)} glyphs, {len(small_data)} bytes", file=sys.stderr)

    large_data = render_font(LARGE_CHARS, font_path, LARGE_PT, LARGE_W, LARGE_H)
    print(f"large font: {len(LARGE_CHARS)} glyphs, {len(large_data)} bytes", file=sys.stderr)

    # Battery icon: 16x10 outline + terminal stub
    battery = bytes([
        0x7F, 0xFC,
        0x80, 0x02,
        0x80, 0x02,
        0x80, 0x02,
        0x80, 0x03,
        0x80, 0x03,
        0x80, 0x02,
        0x80, 0x02,
        0x80, 0x02,
        0x7F, 0xFC,
    ])

    # WiFi icon: 12x10 signal arcs (stylised)
    wifi = bytes([
        0x00, 0x00,
        0x1F, 0xF0,
        0x60, 0x18,
        0x80, 0x04,
        0x1F, 0xF0,
        0x60, 0x18,
        0x07, 0xC0,
        0x18, 0x30,
        0x01, 0x00,
        0x00, 0x00,
    ])

    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w") as f:
        f.write("#ifndef DISPLAY_ASSETS_H\n#define DISPLAY_ASSETS_H\n\n")
        f.write("// Auto-generated by tools/gen_display_assets.py. Do not edit by hand.\n\n")
        f.write("#include <stdint.h>\n\n")

        f.write(f"#define DISPLAY_QR_W   {qr_w}\n")
        f.write(f"#define DISPLAY_QR_H   {qr_h}\n")
        f.write(f"#define DISPLAY_QR_BPR {qr_bpr}\n")
        f.write(emit_array("display_qr", qr_data, f"QR code for {PORTAL_URL}"))

        f.write(f"#define DISPLAY_FONT_SMALL_W     {SMALL_W}\n")
        f.write(f"#define DISPLAY_FONT_SMALL_H     {SMALL_H}\n")
        f.write(f"#define DISPLAY_FONT_SMALL_FIRST 32\n")
        f.write(f"#define DISPLAY_FONT_SMALL_COUNT {len(small_chars)}\n")
        f.write(emit_array("display_font_small", small_data,
                           f"Small font {SMALL_W}x{SMALL_H}, ASCII 32-126"))

        f.write(f"#define DISPLAY_FONT_LARGE_W {LARGE_W}\n")
        f.write(f"#define DISPLAY_FONT_LARGE_H {LARGE_H}\n")
        f.write(f'static const char DISPLAY_FONT_LARGE_CHARS[] = "{LARGE_CHARS}";\n')
        f.write(emit_array("display_font_large", large_data,
                           f"Large font {LARGE_W}x{LARGE_H}, '{LARGE_CHARS}'"))

        f.write("#define DISPLAY_ICON_BATTERY_W 16\n")
        f.write("#define DISPLAY_ICON_BATTERY_H 10\n")
        f.write(emit_array("display_icon_battery", battery, "Battery icon"))

        f.write("#define DISPLAY_ICON_WIFI_W 12\n")
        f.write("#define DISPLAY_ICON_WIFI_H 10\n")
        f.write(emit_array("display_icon_wifi", wifi, "WiFi icon"))

        f.write("#endif // DISPLAY_ASSETS_H\n")
    print(f"wrote {OUT_PATH}", file=sys.stderr)


if __name__ == "__main__":
    main()
