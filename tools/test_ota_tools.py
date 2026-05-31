import struct, subprocess, sys
from pathlib import Path

HERE = Path(__file__).parent
MAKE = HERE / "make_ota_image.py"
OTA_MAGIC = 0x0BEEF11E

def test_make_ota_image_header(tmp_path):
    app = tmp_path / "firmware.bin"
    app.write_bytes(b"\xAA" * 1000)
    out = tmp_path / "out.ota"
    subprocess.run([sys.executable, str(MAKE),
                    "--in", str(app), "--out", str(out),
                    "--manufacturer", "0xFEFE", "--image-type", "0x0001",
                    "--file-version", "0x01020300"], check=True)
    data = out.read_bytes()
    magic, hdr_ver, hdr_len, field_ctrl = struct.unpack("<IHHH", data[:10])
    assert magic == OTA_MAGIC
    assert hdr_len == 56
    assert field_ctrl == 0
    manuf, itype, fver, stackver = struct.unpack("<HHIH", data[10:20])
    assert manuf == 0xFEFE
    assert itype == 0x0001
    assert fver == 0x01020300
    total = struct.unpack("<I", data[52:56])[0]
    assert total == len(data)
    tag, length = struct.unpack("<HI", data[56:62])
    assert tag == 0x0000
    assert length == 1000
    assert data[62:] == b"\xAA" * 1000
