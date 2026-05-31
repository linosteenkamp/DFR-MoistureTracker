#!/usr/bin/env python3
"""Wrap an ESP-IDF firmware.bin into a Zigbee OTA Upgrade Image (.ota).

Produces the standard Zigbee OTA Upgrade Image format (ZCL spec section 11.4)
with field_control = 0 (56-byte header, no optional fields) followed by a
single Upgrade Image sub-element (tag 0x0000).

Usage:
    python3 make_ota_image.py \
        --in firmware.bin --out firmware.ota \
        --manufacturer 0xFEFE --image-type 0x0001 --file-version 0x01020300
"""

import argparse
import struct
from pathlib import Path

OTA_FILE_MAGIC = 0x0BEEF11E
HEADER_LEN = 56
ZIGBEE_STACK_VER = 0x0002
DEFAULT_HEADER_STR = "DFR-SoilSensor OTA"

# Sub-element tag for the upgrade image payload
TAG_UPGRADE_IMAGE = 0x0000


def build(app: bytes, manufacturer: int, image_type: int, file_version: int,
          header_str: str = DEFAULT_HEADER_STR) -> bytes:
    """Build a complete Zigbee OTA image from raw application bytes.

    Header layout (56 bytes, little-endian):
        Offset  Size  Field
         0       4    OTA file identifier (magic)  0x0BEEF11E
         4       2    Header version               0x0100
         6       2    Header length                56
         8       2    Field control                0x0000
        10       2    Manufacturer code
        12       2    Image type
        14       4    File version
        18       2    Zigbee stack version         0x0002
        20      32    Header string (UTF-8, null-padded)
        52       4    Total image size

    Followed by one sub-element:
         0       2    Tag id       0x0000
         4       4    Length       len(app)
         8+      ...  Data         app bytes
    """
    # Build sub-element: 2-byte tag + 4-byte length + payload
    sub_element = struct.pack("<HI", TAG_UPGRADE_IMAGE, len(app)) + app

    total = HEADER_LEN + len(sub_element)

    # 32-byte null-padded header string
    s = header_str.encode("utf-8")[:32].ljust(32, b"\x00")

    # Pack the 56-byte header:
    # <I  = OTA_FILE_MAGIC          (4)
    #  H  = header version          (2)
    #  H  = header length           (2)
    #  H  = field control           (2)
    #  H  = manufacturer code       (2)
    #  H  = image type              (2)
    #  I  = file version            (4)
    #  H  = zigbee stack version    (2)
    # 32s = header string           (32)
    #  I  = total image size        (4)
    # -------------------------------------
    # Total:                        56 bytes
    header = struct.pack(
        "<IHHHHHIH32sI",
        OTA_FILE_MAGIC,
        0x0100,
        HEADER_LEN,
        0x0000,
        manufacturer,
        image_type,
        file_version,
        ZIGBEE_STACK_VER,
        s,
        total,
    )

    assert len(header) == HEADER_LEN, (
        f"Header packing bug: expected {HEADER_LEN} bytes, got {len(header)}"
    )

    return header + sub_element


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Wrap a firmware binary into a Zigbee OTA Upgrade Image."
    )
    parser.add_argument("--in", dest="input", required=True,
                        help="Path to input firmware.bin")
    parser.add_argument("--out", required=True,
                        help="Path to write output .ota file")
    parser.add_argument("--manufacturer", required=True,
                        type=lambda x: int(x, 0),
                        help="Zigbee manufacturer code (e.g. 0xFEFE)")
    parser.add_argument("--image-type", required=True,
                        type=lambda x: int(x, 0),
                        help="OTA image type (e.g. 0x0001)")
    parser.add_argument("--file-version", required=True,
                        type=lambda x: int(x, 0),
                        help="32-bit file version (e.g. 0x01020300)")
    parser.add_argument("--header-string", default=DEFAULT_HEADER_STR,
                        help=f"Header string, max 32 chars (default: '{DEFAULT_HEADER_STR}')")
    args = parser.parse_args()

    app = Path(args.input).read_bytes()
    ota = build(app, args.manufacturer, args.image_type, args.file_version,
                args.header_string)
    Path(args.out).write_bytes(ota)
    print(f"Written {len(ota)} bytes -> {args.out}  "
          f"(header={HEADER_LEN}, payload={len(app)})")


if __name__ == "__main__":
    main()
