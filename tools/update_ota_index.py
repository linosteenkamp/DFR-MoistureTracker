#!/usr/bin/env python3
"""Add/replace this model's entry in the z2m OTA index JSON."""
import argparse, hashlib, json
from pathlib import Path

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--index", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--manufacturer", required=True)
    ap.add_argument("--image-type", required=True)
    ap.add_argument("--file-version", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--image", required=True)
    a = ap.parse_args()

    blob = Path(a.image).read_bytes()
    entry = {
        "modelId": a.model,
        "manufacturerCode": int(a.manufacturer, 0),
        "imageType": int(a.image_type, 0),
        "fileVersion": int(a.file_version, 0),
        "url": a.url,
        "imageSize": len(blob),
        "sha512": hashlib.sha512(blob).hexdigest(),
    }

    p = Path(a.index)
    entries = json.loads(p.read_text()) if p.exists() and p.read_text().strip() else []
    entries = [e for e in entries if e.get("modelId") != a.model]
    entries.append(entry)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(entries, indent=2) + "\n")
    print(f"index now has {len(entries)} entr(ies); {a.model} -> {a.file_version}")

if __name__ == "__main__":
    main()
