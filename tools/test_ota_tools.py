import json, struct, subprocess, sys
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

def test_update_index_replaces_same_model(tmp_path):
    idx = tmp_path / "index.json"
    img = tmp_path / "fw.ota"
    img.write_bytes(b"\x00" * 200)
    args = [sys.executable, str(HERE / "update_ota_index.py"),
            "--index", str(idx), "--model", "DFR-SoilSensor",
            "--manufacturer", "0xFEFE", "--image-type", "0x0001",
            "--url", "https://example/fw.ota", "--image", str(img)]
    subprocess.run(args + ["--file-version", "0x01000000"], check=True)
    subprocess.run(args + ["--file-version", "0x01000100"], check=True)
    entries = json.loads(idx.read_text())
    mine = [e for e in entries if e["modelId"] == "DFR-SoilSensor"]
    assert len(mine) == 1                       # replaced, not appended
    assert mine[0]["fileVersion"] == 0x01000100
    assert mine[0]["imageSize"] == 200
    assert mine[0]["url"] == "https://example/fw.ota"
