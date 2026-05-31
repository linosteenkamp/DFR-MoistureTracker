# Zigbee OTA Firmware Updates — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver firmware to deployed solar soil sensors remotely over Zigbee via zigbee2mqtt, with binaries built and published by GitHub Actions.

**Architecture:** The device gains an OTA Upgrade client cluster (0x0019) and a dual-OTA partition layout. A GitHub Actions workflow builds the firmware on a `v*` tag, wraps it into a Zigbee `.ota` image, publishes it to a GitHub Release, and updates a z2m OTA index JSON. z2m offers the image; the operator triggers updates per-device (staged). During a download the sleepy end-device switches to a "burst mode" (rx-on, no light sleep, fast poll) so transfers take minutes; a fresh image must rejoin and report once or the bootloader rolls back.

**Tech Stack:** ESP-IDF (esp-zigbee-lib 1.6.x, `esp_ota_*`), PlatformIO, Python 3 (build tools + pytest), GitHub Actions, zigbee2mqtt external converter (JS).

**Reference spec:** `docs/superpowers/specs/2026-05-31-zigbee-ota-design.md`

**Testing reality:** This project has no on-target test framework (per `CLAUDE.md`); firmware validation is manual via serial monitor + z2m/MQTT. Pure logic (version packing, the Python tools) IS unit-tested (`pio test -e native`, `pytest`). Firmware tasks below verify by **build + flash + observe**; the final task is a manual on-device checklist.

---

## File structure

| File | Create/Modify | Responsibility |
|------|---------------|----------------|
| `docs/ota-api-notes.md` | Create | Task 1 output: confirmed esp-zigbee OTA client API |
| `partitions.csv` | Modify | Dual-OTA layout (otadata + ota_0/ota_1) |
| `sdkconfig.defaults.zigbee` | Modify | Enable app rollback |
| `include/ota_ids.h` | Create | Fixed product constants + version-pack helper (host-safe) |
| `test/test_ota_version/test_ota_version.c` | Create | Native unit test for version packing |
| `include/fw_version.h` | Create | Build-injected firmware version (uint32 + string) |
| `include/ota_client.h` / `src/ota_client.c` | Create | OTA Upgrade client: cluster cfg, callback, block write, burst mode |
| `src/zigbee_reporter.c` | Modify | Add OTA cluster to endpoint; expose report-pause + sleep-control hooks |
| `src/main.c` | Modify | Rollback self-check at boot; mark-valid after first good report |
| `src/CMakeLists.txt` | Modify | Register `ota_client.c` |
| `tools/make_ota_image.py` | Create | Wrap `firmware.bin` → `.ota` |
| `tools/update_ota_index.py` | Create | Maintain `ota/index.json` |
| `tools/test_ota_tools.py` | Create | pytest for the two tools |
| `ota/index.json` | Create | Seed OTA index |
| `z2m/dfr_soil_moisture.js` | Modify | Declare OTA support |
| `.github/workflows/release-ota.yml` | Create | Build → wrap → Release → index on `v*` tag |
| `DEVELOPER_GUIDE.md` | Modify | Bootstrap / release / canary / fleet / rollback procedure |

---

### Task 1: Confirm the esp-zigbee 1.6.x OTA client API

De-risk first: the exact callback message struct field names must be read from the installed header before writing `ota_client.c`.

**Files:**
- Create: `docs/ota-api-notes.md`
- Read: `~/.platformio/packages/framework-arduinoespressif32-libs/esp32c6/include/espressif__esp-zigbee-lib/include/esp_zigbee_ota.h`

- [ ] **Step 1: Read the OTA header and the core-action callback message type**

Run:
```bash
HDR=~/.platformio/packages/framework-arduinoespressif32-libs/esp32c6/include/espressif__esp-zigbee-lib/include/esp_zigbee_ota.h
sed -n '1,200p' "$HDR"
grep -rn "esp_zb_zcl_ota_upgrade_value_message_t\|ota_upgrade_status_t\|esp_zb_ota_cluster_cfg_t\|query_interval_set\|query_image_req" \
  ~/.platformio/packages/framework-arduinoespressif32-libs/esp32c6/include/espressif__esp-zigbee-lib/include/*.h
```

- [ ] **Step 2: Record the confirmed API in `docs/ota-api-notes.md`**

Document exactly (copy real signatures/fields):
- `esp_zb_ota_cluster_cfg_t` fields (ota_upgrade_file_version, ota_upgrade_manufacturer, ota_upgrade_image_type, hardware/stack version, downloaded versions, etc.).
- `esp_zb_ota_cluster_create()` / `esp_zb_ota_cluster_add_attr()` signatures.
- `esp_zb_ota_upgrade_client_query_interval_set(endpoint, interval)` and `esp_zb_ota_upgrade_client_query_image_req(...)` signatures.
- The callback: how `ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID` is delivered via `esp_zb_core_action_handler_register(...)`, and the **exact** message struct (likely `esp_zb_zcl_ota_upgrade_value_message_t`) with its fields: `upgrade_status`, `ota_header`, `payload_size`, `payload`, and the return-status convention.
- The `ESP_ZB_ZCL_OTA_UPGRADE_STATUS_*` values: START, RECEIVE, APPLY, CHECK, FINISH, ABORT, OK, ERROR.

- [ ] **Step 3: Commit**

```bash
git add docs/ota-api-notes.md
git commit -m "docs: confirm esp-zigbee 1.6.x OTA client API surface"
```

> Tasks 9–12 use the names recorded here. If a name differs from what this plan assumes, prefer the header.

---

### Task 2: Dual-OTA partition table + rollback config

**Files:**
- Modify: `partitions.csv`
- Modify: `sdkconfig.defaults.zigbee`

- [ ] **Step 1: Rewrite `partitions.csv` to the dual-OTA layout**

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,        0x6000,
phy_init, data, phy,     ,        0x1000,
otadata,  data, ota,     ,        0x2000,
ota_0,    app,  ota_0,   ,        0x180000,
ota_1,    app,  ota_1,   ,        0x180000,
storage,  data, nvs,     ,        0x4000,
zb_storage, data, fat,   ,        0x4000,
zb_fct,   data, fat,     ,        0x400,
```

- [ ] **Step 2: Enable app rollback in `sdkconfig.defaults.zigbee`**

Append:
```
# OTA: roll back to the previous app if a freshly-flashed image fails its
# self-check (see ota_client / main.c rollback gate).
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

- [ ] **Step 3: Force a clean sdkconfig regen and build**

Run:
```bash
rm -f sdkconfig.dfrobot_firebeetle2_esp32c6_zigbee sdkconfig.dfrobot_firebeetle2_esp32c6_zigbee_test
pio run -e dfrobot_firebeetle2_esp32c6_zigbee
```
Expected: SUCCESS. Confirm the app now targets an OTA slot:
```bash
grep -E "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" sdkconfig.dfrobot_firebeetle2_esp32c6_zigbee
```
Expected: the line prints.

- [ ] **Step 4: Commit**

```bash
git add partitions.csv sdkconfig.defaults.zigbee sdkconfig.dfrobot_firebeetle2_esp32c6_zigbee sdkconfig.dfrobot_firebeetle2_esp32c6_zigbee_test
git commit -m "ota: dual-OTA partition table + app rollback"
```

> NOTE for execution: this partition change means the next on-device flash must be over USB and the node re-pairs. That happens in Task 14, not here.

---

### Task 3: Shared OTA identity constants + version packing (host-tested)

**Files:**
- Create: `include/ota_ids.h`
- Test: `test/test_ota_version/test_ota_version.c`
- Modify: `platformio.ini` (add `test_ota_version` to native `test_filter`)

- [ ] **Step 1: Write the failing native test**

`test/test_ota_version/test_ota_version.c`:
```c
#include <unity.h>
#include "ota_ids.h"

void setUp(void) {}
void tearDown(void) {}

void test_pack_version_orders_bytes_major_minor_patch(void) {
    // v1.2.3 -> 0x01020300
    TEST_ASSERT_EQUAL_HEX32(0x01020300u, OTA_PACK_VERSION(1, 2, 3, 0));
}

void test_pack_version_includes_build_byte(void) {
    TEST_ASSERT_EQUAL_HEX32(0x0102030Au, OTA_PACK_VERSION(1, 2, 3, 10));
}

void test_higher_semver_is_greater_uint32(void) {
    TEST_ASSERT_TRUE(OTA_PACK_VERSION(1, 3, 0, 0) > OTA_PACK_VERSION(1, 2, 9, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pack_version_orders_bytes_major_minor_patch);
    RUN_TEST(test_pack_version_includes_build_byte);
    RUN_TEST(test_higher_semver_is_greater_uint32);
    return UNITY_END();
}
```

- [ ] **Step 2: Add the test to the native env in `platformio.ini`**

Under `[env:native]` `test_filter`, add a line:
```
    test_ota_version
```

- [ ] **Step 3: Run the test, verify it fails**

Run: `pio test -e native -f test_ota_version`
Expected: FAIL — `ota_ids.h` not found.

- [ ] **Step 4: Create `include/ota_ids.h`**

```c
#ifndef OTA_IDS_H
#define OTA_IDS_H

#include <stdint.h>

/* Fixed product identity for Zigbee OTA. These three values MUST match across
 * the firmware, the .ota image header, and the z2m OTA index. */
#define OTA_MANUFACTURER_CODE  0xFEFEu   /* chosen DIY 16-bit code */
#define OTA_IMAGE_TYPE         0x0001u
#define OTA_MODEL_ID           "DFR-SoilSensor"

/* Pack semver into the 32-bit Zigbee fileVersion as 0xMMmmpp bb
 * (major, minor, patch, build). Monotonic: higher semver => larger uint32. */
#define OTA_PACK_VERSION(major, minor, patch, build)            \
    (((uint32_t)(major) << 24) | ((uint32_t)(minor) << 16) |    \
     ((uint32_t)(patch) << 8)  |  (uint32_t)(build))

#endif /* OTA_IDS_H */
```

- [ ] **Step 5: Run the test, verify it passes**

Run: `pio test -e native -f test_ota_version`
Expected: PASS (3 tests).

- [ ] **Step 6: Commit**

```bash
git add include/ota_ids.h test/test_ota_version/test_ota_version.c platformio.ini
git commit -m "ota: shared identity constants + version-pack helper (host-tested)"
```

---

### Task 4: Build-injected firmware version

**Files:**
- Create: `include/fw_version.h`
- Modify: `platformio.ini` (zigbee env build_flags)

- [ ] **Step 1: Create `include/fw_version.h`**

```c
#ifndef FW_VERSION_H
#define FW_VERSION_H

#include "ota_ids.h"

/* FW_VER_MAJOR/MINOR/PATCH/BUILD are injected by the build (-D flags) from the
 * git tag in CI. Defaults keep local dev builds compiling. */
#ifndef FW_VER_MAJOR
#define FW_VER_MAJOR 0
#endif
#ifndef FW_VER_MINOR
#define FW_VER_MINOR 0
#endif
#ifndef FW_VER_PATCH
#define FW_VER_PATCH 0
#endif
#ifndef FW_VER_BUILD
#define FW_VER_BUILD 0
#endif

#define FW_VERSION_U32  OTA_PACK_VERSION(FW_VER_MAJOR, FW_VER_MINOR, FW_VER_PATCH, FW_VER_BUILD)

#define FW_VER_STR_(a,b,c) "v" #a "." #b "." #c
#define FW_VER_STR__(a,b,c) FW_VER_STR_(a,b,c)
#define FW_VERSION_STR  FW_VER_STR__(FW_VER_MAJOR, FW_VER_MINOR, FW_VER_PATCH)

#endif /* FW_VERSION_H */
```

- [ ] **Step 2: Document the CI override in `platformio.ini`**

Under `[env:dfrobot_firebeetle2_esp32c6_zigbee]`, add a comment after `build_flags`:
```ini
; CI injects the version from the git tag, e.g.:
;   PLATFORMIO_BUILD_FLAGS="-DFW_VER_MAJOR=1 -DFW_VER_MINOR=2 -DFW_VER_PATCH=3"
```

- [ ] **Step 3: Build to confirm defaults compile**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add include/fw_version.h platformio.ini
git commit -m "ota: build-injected firmware version (fw_version.h)"
```

---

### Task 5: `.ota` image wrap tool (host-tested)

The Zigbee OTA Upgrade file = a 56-byte+ OTA header followed by one "Upgrade Image" sub-element (tag `0x0000`, 4-byte length, then the app binary).

**Files:**
- Create: `tools/make_ota_image.py`
- Test: `tools/test_ota_tools.py`

- [ ] **Step 1: Write the failing test**

`tools/test_ota_tools.py`:
```python
import struct, subprocess, sys, json
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
    magic, hdr_ver, hdr_len = struct.unpack("<IHH", data[:8])
    assert magic == OTA_MAGIC
    # manufacturer (offset 10), image type (12), file version (14)
    manuf, itype, fver = struct.unpack("<HHI", data[10:18])
    assert manuf == 0xFEFE
    assert itype == 0x0001
    assert fver == 0x01020300
    # total size field (offset 52) == len(file)
    total = struct.unpack("<I", data[52:56])[0]
    assert total == len(data)
    # sub-element tag 0x0000 + length == app size, then app bytes
    tag, length = struct.unpack("<HI", data[hdr_len:hdr_len+6])
    assert tag == 0x0000
    assert length == 1000
    assert data[hdr_len+6:] == b"\xAA" * 1000
```

- [ ] **Step 2: Run the test, verify it fails**

Run: `python -m pytest tools/test_ota_tools.py -k make_ota_image -v`
Expected: FAIL — `make_ota_image.py` missing.

- [ ] **Step 3: Implement `tools/make_ota_image.py`**

```python
#!/usr/bin/env python3
"""Wrap an ESP-IDF app .bin into a Zigbee OTA Upgrade image (.ota)."""
import argparse, struct

OTA_FILE_MAGIC = 0x0BEEF11E
OTA_HEADER_VERSION = 0x0100
TAG_UPGRADE_IMAGE = 0x0000

def build(app: bytes, manufacturer: int, image_type: int, file_version: int,
          header_str: str = "DFR-SoilSensor OTA") -> bytes:
    sub = struct.pack("<HI", TAG_UPGRADE_IMAGE, len(app)) + app
    hdr_str = header_str.encode("utf-8")[:32].ljust(32, b"\x00")
    header_len = 56
    total_size = header_len + len(sub)
    header = struct.pack(
        "<IHHHHHI32sIBHHII",
        OTA_FILE_MAGIC,     # file identifier
        OTA_HEADER_VERSION, # header version
        header_len,         # header length
        0x0000,             # field control
        manufacturer,       # manufacturer code
        image_type,         # image type
        file_version,       # file version
        hdr_str,            # 32-byte header string
        total_size,         # total image size
        0, 0, 0,            # security cred ver(1), upgrade dependency? (kept 0)
        0, 0,               # min/max hw version placeholders (field control 0 => absent on wire for real stacks; kept here as zero padding to reach 56)
    )
    header = header[:header_len].ljust(header_len, b"\x00")
    # patch total_size at offset 52 in case packing/truncation shifted it
    header = header[:52] + struct.pack("<I", total_size) + header[56:]
    return header + sub

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--manufacturer", required=True)
    ap.add_argument("--image-type", required=True)
    ap.add_argument("--file-version", required=True)
    a = ap.parse_args()
    app = open(a.inp, "rb").read()
    img = build(app, int(a.manufacturer, 0), int(a.image_type, 0), int(a.file_version, 0))
    open(a.out, "wb").write(img)
    print(f"wrote {a.out}: {len(img)} bytes, fileVersion {a.file_version}")

if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run the test, verify it passes**

Run: `python -m pytest tools/test_ota_tools.py -k make_ota_image -v`
Expected: PASS.

> NOTE: the exact OTA header byte layout must match what z2m's parser expects. Task 14 includes a real z2m ingest check; if z2m rejects the header, adjust `build()` field offsets to the Zigbee OTA spec (the test asserts the offsets this implementation uses, so update both together).

- [ ] **Step 5: Commit**

```bash
git add tools/make_ota_image.py tools/test_ota_tools.py
git commit -m "ota: .ota image wrap tool (host-tested)"
```

---

### Task 6: OTA index generator (host-tested)

**Files:**
- Create: `tools/update_ota_index.py`
- Modify: `tools/test_ota_tools.py` (add test)

- [ ] **Step 1: Add the failing test to `tools/test_ota_tools.py`**

```python
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
```

- [ ] **Step 2: Run the test, verify it fails**

Run: `python -m pytest tools/test_ota_tools.py -k update_index -v`
Expected: FAIL — `update_ota_index.py` missing.

- [ ] **Step 3: Implement `tools/update_ota_index.py`**

```python
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
```

- [ ] **Step 4: Run the test, verify it passes**

Run: `python -m pytest tools/test_ota_tools.py -v`
Expected: PASS (both tools' tests).

- [ ] **Step 5: Commit**

```bash
git add tools/update_ota_index.py tools/test_ota_tools.py
git commit -m "ota: index generator (host-tested)"
```

---

### Task 7: Seed index + z2m converter OTA support

**Files:**
- Create: `ota/index.json`
- Modify: `z2m/dfr_soil_moisture.js`

- [ ] **Step 1: Create `ota/index.json` (empty seed)**

```json
[]
```

- [ ] **Step 2: Add OTA support to the converter**

In `z2m/dfr_soil_moisture.js`, add `ota` to the imports and the definition. Add near the other requires:
```js
const ota = require('zigbee-herdsman-converters/lib/ota');
```
And inside the exported definition object (alongside `fromZigbee`/`toZigbee`/`exposes`), add:
```js
    ota: ota.zigbeeOTA,
    // The image is matched from z2m's override index by manufacturerCode 0xFEFE
    // + imageType 0x0001 (see ota/index.json and zigbee_ota_override_index_location).
```

- [ ] **Step 3: Document the z2m configuration in the converter header comment**

At the top of `z2m/dfr_soil_moisture.js`, add a comment block:
```js
// OTA: in zigbee2mqtt configuration.yaml set:
//   ota:
//     zigbee_ota_override_index_location: https://raw.githubusercontent.com/<owner>/DFR-MoistureTracker/master/ota/index.json
//     disable_automatic_update_check: true   # staged/manual rollout
// Trigger updates per device in the z2m UI (device -> OTA -> Update).
```

- [ ] **Step 4: Commit**

```bash
git add ota/index.json z2m/dfr_soil_moisture.js
git commit -m "ota: seed index + z2m converter OTA support"
```

---

### Task 8: GitHub Actions release workflow

**Files:**
- Create: `.github/workflows/release-ota.yml`

- [ ] **Step 1: Create the workflow**

```yaml
name: Release OTA
on:
  push:
    tags: ['v*']

permissions:
  contents: write

env:
  OTA_MANUFACTURER_CODE: "0xFEFE"
  OTA_IMAGE_TYPE: "0x0001"
  MODEL_ID: "DFR-SoilSensor"

jobs:
  build-and-release:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with: { ref: master, fetch-depth: 0 }

      - name: Derive version from tag
        id: ver
        run: |
          TAG="${GITHUB_REF_NAME#v}"
          IFS=. read -r MA MI PA <<< "$TAG"
          printf 'file_version=0x%02X%02X%02X00\n' "$MA" "$MI" "$PA" >> "$GITHUB_OUTPUT"
          echo "semver=$TAG major=$MA minor=$MI patch=$PA" >> "$GITHUB_OUTPUT"
          echo "ma=$MA" >> "$GITHUB_OUTPUT"; echo "mi=$MI" >> "$GITHUB_OUTPUT"; echo "pa=$PA" >> "$GITHUB_OUTPUT"

      - uses: actions/setup-python@v5
        with: { python-version: '3.x' }
      - run: pip install platformio pytest

      - name: Unit tests (host tools + native)
        run: |
          python -m pytest tools/test_ota_tools.py -v
          pio test -e native

      - name: Build Zigbee firmware (version injected)
        run: |
          cp include/mqtt_credentials.h.example include/mqtt_credentials.h
          PLATFORMIO_BUILD_FLAGS="-DFW_VER_MAJOR=${{ steps.ver.outputs.ma }} -DFW_VER_MINOR=${{ steps.ver.outputs.mi }} -DFW_VER_PATCH=${{ steps.ver.outputs.pa }}" \
            pio run -e dfrobot_firebeetle2_esp32c6_zigbee

      - name: Wrap into .ota
        run: |
          python tools/make_ota_image.py \
            --in .pio/build/dfrobot_firebeetle2_esp32c6_zigbee/firmware.bin \
            --out firmware-${{ steps.ver.outputs.semver }}.ota \
            --manufacturer ${{ env.OTA_MANUFACTURER_CODE }} \
            --image-type ${{ env.OTA_IMAGE_TYPE }} \
            --file-version ${{ steps.ver.outputs.file_version }}

      - name: Update index against master tip
        run: |
          python tools/update_ota_index.py \
            --index ota/index.json --model "${{ env.MODEL_ID }}" \
            --manufacturer ${{ env.OTA_MANUFACTURER_CODE }} \
            --image-type ${{ env.OTA_IMAGE_TYPE }} \
            --file-version ${{ steps.ver.outputs.file_version }} \
            --url "https://github.com/${{ github.repository }}/releases/download/${{ github.ref_name }}/firmware-${{ steps.ver.outputs.semver }}.ota" \
            --image firmware-${{ steps.ver.outputs.semver }}.ota

      - name: Create Release with .ota asset
        uses: softprops/action-gh-release@v2
        with:
          files: firmware-${{ steps.ver.outputs.semver }}.ota

      - name: Commit index to master
        run: |
          git config user.name  "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          git add ota/index.json
          git commit -m "ota: index for ${{ github.ref_name }}" || echo "no change"
          git push origin HEAD:master
```

> The `checkout` pins `ref: master`, so the index commit lands on master tip (resolves the detached-HEAD concern from the spec). The Release asset URL is deterministic from the tag, so the index can be generated before the Release exists.

- [ ] **Step 2: Lint the YAML locally (optional) and commit**

```bash
python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/release-ota.yml'))" && echo "yaml ok"
git add .github/workflows/release-ota.yml
git commit -m "ci: release-ota workflow (build, wrap, release, index on v* tag)"
```

---

### Task 9: OTA client module — cluster registration + query interval

**Files:**
- Create: `include/ota_client.h`, `src/ota_client.c`
- Modify: `src/CMakeLists.txt`, `src/zigbee_reporter.c`

> Uses API confirmed in Task 1 / `docs/ota-api-notes.md`. If a symbol differs, prefer the header.

- [ ] **Step 1: Create `include/ota_client.h`**

```c
#ifndef OTA_CLIENT_H
#define OTA_CLIENT_H

#ifdef USE_ZIGBEE
#include "esp_err.h"
#include "esp_zigbee_core.h"

/* Add the OTA Upgrade client cluster to an existing cluster list. Call while
 * building the endpoint in zigbee_reporter's esp_zb_task. */
void ota_client_add_cluster(esp_zb_cluster_list_t *clusters);

/* Called once after the stack starts (post esp_zb_start) to set the periodic
 * image-query interval and kick the first query. */
void ota_client_start(uint8_t endpoint);

/* Boot-time: if running a freshly-OTA'd image (pending-verify), returns true.
 * main.c uses this to arm the rollback self-check. */
bool ota_client_image_pending_verify(void);

/* Call after the device has joined and pushed one good report — confirms the
 * new image so the bootloader keeps it (no-op if not pending-verify). */
void ota_client_mark_valid(void);

#endif /* USE_ZIGBEE */
#endif /* OTA_CLIENT_H */
```

- [ ] **Step 2: Create `src/ota_client.c` (cluster + start + rollback helpers; block handling stubbed for Task 10)**

```c
#ifdef USE_ZIGBEE
#include "ota_client.h"
#include "ota_ids.h"
#include "fw_version.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_ota.h"
#include "esp_ota_ops.h"
#include "esp_log.h"

static const char *TAG = "OTA_CLI";

#define OTA_QUERY_INTERVAL_MIN  30   /* minutes between image-version queries */

void ota_client_add_cluster(esp_zb_cluster_list_t *clusters)
{
    esp_zb_ota_cluster_cfg_t ota_cfg = {
        .ota_upgrade_file_version   = FW_VERSION_U32,
        .ota_upgrade_manufacturer   = OTA_MANUFACTURER_CODE,
        .ota_upgrade_image_type     = OTA_IMAGE_TYPE,
        .ota_upgrade_downloaded_file_ver = FW_VERSION_U32,
    };
    esp_zb_attribute_list_t *ota_attrs = esp_zb_ota_cluster_create(&ota_cfg);
    esp_zb_cluster_list_add_ota_cluster(clusters, ota_attrs,
                                        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    ESP_LOGI(TAG, "OTA client cluster added (current fw %s = 0x%08x)",
             FW_VERSION_STR, (unsigned)FW_VERSION_U32);
}

void ota_client_start(uint8_t endpoint)
{
    /* Periodically ask the network's OTA server (z2m) for a newer image. */
    esp_zb_ota_upgrade_client_query_interval_set(endpoint, OTA_QUERY_INTERVAL_MIN);
    esp_zb_ota_upgrade_client_query_image_req(0xFFFF /*server short addr: discover*/, endpoint);
    ESP_LOGI(TAG, "OTA client started (query every %d min)", OTA_QUERY_INTERVAL_MIN);
}

bool ota_client_image_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
        return st == ESP_OTA_IMG_PENDING_VERIFY;
    }
    return false;
}

void ota_client_mark_valid(void)
{
    if (ota_client_image_pending_verify()) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "New image confirmed valid (rollback cancelled)");
    }
}
#endif /* USE_ZIGBEE */
```

> Confirm `esp_zb_cluster_list_add_ota_cluster` and the `esp_zb_ota_cluster_cfg_t` field names against `docs/ota-api-notes.md`; adjust if the header differs.

- [ ] **Step 3: Register `ota_client.c` in `src/CMakeLists.txt`**

Add `"ota_client.c"` to the `SRCS` list.

- [ ] **Step 4: Wire into `zigbee_reporter.c`**

In `esp_zb_task`, after the existing clusters are added to `clusters` and before `esp_zb_device_register`, add:
```c
#include "ota_client.h"   /* near the other includes */
...
    ota_client_add_cluster(clusters);   /* OTA Upgrade client (0x0019) */
```
After `ESP_ERROR_CHECK(esp_zb_start(false));` add:
```c
    ota_client_start(APP_ENDPOINT);
```

- [ ] **Step 5: Build**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: SUCCESS. (If a symbol name mismatches the header, fix per Task 1 notes.)

- [ ] **Step 6: Commit**

```bash
git add include/ota_client.h src/ota_client.c src/CMakeLists.txt src/zigbee_reporter.c
git commit -m "ota: register OTA Upgrade client cluster + periodic query"
```

---

### Task 10: Receive blocks and apply the image

**Files:**
- Modify: `src/ota_client.c`

- [ ] **Step 1: Add the OTA value callback (block write + finish) to `ota_client.c`**

Add includes and state:
```c
#include "esp_app_format.h"

static const esp_partition_t *s_ota_part = NULL;
static esp_ota_handle_t       s_ota_handle = 0;
static bool                   s_ota_in_progress = false;
```

Add the handler (called from the core action handler — see Step 2):
```c
/* esp_err_t return: ESP_OK accepts the block. Field names per ota-api-notes.md. */
esp_err_t ota_client_on_value(const esp_zb_zcl_ota_upgrade_value_message_t *msg)
{
    switch (msg->upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
        s_ota_part = esp_ota_get_next_update_partition(NULL);
        ESP_LOGI(TAG, "OTA start -> slot %s", s_ota_part ? s_ota_part->label : "?");
        if (!s_ota_part ||
            esp_ota_begin(s_ota_part, OTA_SIZE_UNKNOWN, &s_ota_handle) != ESP_OK) {
            return ESP_FAIL;
        }
        s_ota_in_progress = true;
        ota_client_burst_begin();           /* Task 11 */
        return ESP_OK;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
        if (!s_ota_in_progress) return ESP_FAIL;
        ota_client_burst_kick();            /* Task 11: feed the stall watchdog */
        return esp_ota_write(s_ota_handle, msg->payload, msg->payload_size);

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
        return ESP_OK;                      /* allow apply */

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
        if (esp_ota_end(s_ota_handle) != ESP_OK ||
            esp_ota_set_boot_partition(s_ota_part) != ESP_OK) {
            ESP_LOGE(TAG, "OTA finalize failed");
            s_ota_in_progress = false;
            ota_client_burst_end();
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "OTA complete — rebooting into new image");
        esp_restart();
        return ESP_OK;                      /* not reached */

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR:
    default:
        if (s_ota_in_progress) {
            esp_ota_abort(s_ota_handle);
            s_ota_in_progress = false;
            ota_client_burst_end();
        }
        ESP_LOGW(TAG, "OTA aborted (status %d)", msg->upgrade_status);
        return ESP_OK;
    }
}
```

- [ ] **Step 2: Register the core action handler in `ota_client_start`**

In `zigbee_reporter.c`'s `esp_zb_app_signal_handler` setup path (or in `esp_zb_task` before `esp_zb_start`), ensure the core action handler is registered and routes OTA to `ota_client_on_value`. Add to `ota_client.c`:
```c
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t cb_id, const void *message)
{
    if (cb_id == ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID) {
        return ota_client_on_value((const esp_zb_zcl_ota_upgrade_value_message_t *)message);
    }
    return ESP_OK;
}
```
and in `ota_client_start`, before the query req:
```c
    esp_zb_core_action_handler_register(zb_action_handler);
```
Declare `esp_err_t ota_client_on_value(...)` and `static esp_err_t zb_action_handler(...)` above their use.

> If `zigbee_reporter.c` already registers a core action handler, merge: route OTA cb_id to `ota_client_on_value` from the existing handler instead of registering a second one.

- [ ] **Step 3: Temporarily stub the burst hooks so it builds (real impl in Task 11)**

Add to `ota_client.c` (will be replaced in Task 11):
```c
void ota_client_burst_begin(void) {}
void ota_client_burst_kick(void)  {}
void ota_client_burst_end(void)   {}
```

- [ ] **Step 4: Build**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add src/ota_client.c src/zigbee_reporter.c
git commit -m "ota: receive image blocks, write to inactive slot, reboot on finish"
```

---

### Task 11: Burst mode (rx-on, no light sleep, pause reports, stall watchdog)

**Files:**
- Modify: `src/ota_client.c`, `src/zigbee_reporter.c`, `src/main.c`, `include/ota_client.h`

- [ ] **Step 1: Expose report-task pause + sleep control from zigbee_reporter / main**

In `zigbee_reporter.h` declare and in `zigbee_reporter.c` implement:
```c
void zigbee_reporter_set_reports_paused(bool paused);   /* gate periodic_report_cb work */
```
Implement with a `static volatile bool s_reports_paused;` checked at the top of the periodic tick path (skip sampling/report when paused).

In `main.c` expose a setter for the report task to honor pause (the `zb_report_task` checks `zigbee_reporter` pause flag before sampling), so an in-flight OTA halts ADC/e-paper work.

- [ ] **Step 2: Replace the burst stubs in `ota_client.c` with real behavior**

```c
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "zigbee_reporter.h"

#define OTA_STALL_TIMEOUT_MS  60000

static TimerHandle_t s_stall_timer = NULL;

static void ota_stall_cb(TimerHandle_t t) {
    (void)t;
    ESP_LOGW(TAG, "OTA stalled — aborting, returning to sleepy mode");
    if (s_ota_in_progress) { esp_ota_abort(s_ota_handle); s_ota_in_progress = false; }
    ota_client_burst_end();
}

void ota_client_burst_begin(void) {
    zigbee_reporter_set_reports_paused(true);   /* no ADC/e-paper mid-OTA */
    esp_zb_sleep_enable(false);                 /* no light sleep during download */
    esp_zb_set_rx_on_when_idle(true);           /* keep receiver up */
    esp_zb_zdo_pim_set_long_poll_interval(1);   /* fast poll (seconds) — confirm name in notes */
    if (!s_stall_timer) {
        s_stall_timer = xTimerCreate("ota_stall", pdMS_TO_TICKS(OTA_STALL_TIMEOUT_MS),
                                     pdFALSE, NULL, ota_stall_cb);
    }
    xTimerStart(s_stall_timer, 0);
    ESP_LOGI(TAG, "burst mode ON");
}

void ota_client_burst_kick(void) {
    if (s_stall_timer) xTimerReset(s_stall_timer, 0);
}

void ota_client_burst_end(void) {
    if (s_stall_timer) xTimerStop(s_stall_timer, 0);
    esp_zb_set_rx_on_when_idle(false);          /* back to sleepy ED */
    esp_zb_zdo_pim_set_long_poll_interval(15);  /* restore 15 s poll */
    esp_zb_sleep_enable(true);
    zigbee_reporter_set_reports_paused(false);
    ESP_LOGI(TAG, "burst mode OFF");
}
```

> The poll-interval setter name (`esp_zb_zdo_pim_set_long_poll_interval`) must be confirmed in Task 1 notes; if unavailable in 1.6.x, rely on rx-on + `keep_alive` to keep blocks flowing and drop these two lines.

- [ ] **Step 3: Remove the stub definitions added in Task 10**

Delete the three empty stub functions from Task 10 Step 3.

- [ ] **Step 4: Build**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add src/ota_client.c src/zigbee_reporter.c src/zigbee_reporter.h src/main.c include/ota_client.h
git commit -m "ota: burst mode during download + stall watchdog"
```

---

### Task 12: Rollback self-check at boot

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Arm the self-check and confirm after first good report**

In `main.c`, in the `#ifdef USE_ZIGBEE` path after the stack is up and `zigbee_reporter_wait_ready(...)` returns joined, and after the FIRST successful report cycle, call `ota_client_mark_valid()`. Concretely, in `zb_report_task`, after the first successful `zigbee_reporter_report(...)`:
```c
#include "ota_client.h"
...
        // After the first good report on a freshly-OTA'd image, confirm it so
        // the bootloader keeps the new slot; otherwise it auto-reverts on reboot.
        static bool s_first_report_done = false;
        if (!s_first_report_done) {
            s_first_report_done = true;
            ota_client_mark_valid();
        }
```

- [ ] **Step 2: Log pending-verify state at boot for observability**

In `app_main` (Zigbee path), near startup logging:
```c
    if (ota_client_image_pending_verify()) {
        ESP_LOGW(TAG, "Running a PENDING-VERIFY image — must report once to confirm");
    }
```

- [ ] **Step 3: Build**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add src/main.c
git commit -m "ota: rollback self-check — confirm image after first good report"
```

---

### Task 13: Document the procedure

**Files:**
- Modify: `DEVELOPER_GUIDE.md`

- [ ] **Step 1: Add an "OTA Updates (SP)" section**

Document, with exact commands:
- **Bootstrap (once per node, USB):** `pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t erase -t upload`, then re-pair in z2m. Explain this is required because the partition table changed.
- **z2m config:** the `ota.zigbee_ota_override_index_location` + `disable_automatic_update_check: true` snippet (raw URL of `ota/index.json`).
- **Release:** `git tag vX.Y.Z && git push --tags` → CI publishes the `.ota` + index.
- **Canary:** z2m UI → one device → OTA → Update; watch burst download → reboot → rejoin → report; confirm `vX.Y.Z` and sane soil/battery.
- **Fleet:** repeat Update on remaining devices.
- **Rollback:** a freshly-flashed image that fails to rejoin+report within the timeout auto-reverts; a "boots-but-misbehaves" image is caught on the canary before fleet rollout.

- [ ] **Step 2: Commit**

```bash
git add DEVELOPER_GUIDE.md
git commit -m "docs: OTA update procedure (bootstrap, release, canary, fleet, rollback)"
```

---

### Task 14: On-device end-to-end verification (manual)

No code — this is the acceptance checklist. Requires the board on USB for bootstrap, then on battery/solar.

- [ ] **Step 1: Bootstrap flash + pair**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t erase -t upload -t monitor`
Expected: boots, joins, reports; serial shows `OTA client cluster added` and `OTA client started`. Re-pair in z2m.

- [ ] **Step 2: Publish a test release**

Run: `git tag v0.0.1 && git push --tags`
Expected: GitHub Action succeeds; a Release with `firmware-0.0.1.ota` exists; `ota/index.json` updated on master.

- [ ] **Step 3: Confirm z2m sees the image**

In z2m, point `zigbee_ota_override_index_location` at the raw index URL, restart z2m, open the device's OTA tab. Expected: an update is available (newer `fileVersion`).

- [ ] **Step 4: Trigger the update (canary)**

Click Update. Expected (serial): `OTA start -> slot ota_1`, `burst mode ON`, repeated `RECEIVE`, then `OTA complete — rebooting`. Device reboots, rejoins, logs `PENDING-VERIFY`, pushes a report, logs `New image confirmed valid`. z2m shows the new version; soil/battery sane.

- [ ] **Step 5: Verify rollback**

Build a deliberately-broken image (e.g. a build that fails to join — temporarily wrong channel), publish as `v0.0.2`, update one node. Expected: it flashes, fails the self-check within the timeout, and the bootloader reverts to `v0.0.1` on the next boot (serial shows the old version rejoining). Revert the deliberate break afterward.

- [ ] **Step 6: Final commit (any doc tweaks discovered during verification)**

```bash
git add -A && git commit -m "ota: verification notes/tweaks" || echo "nothing to commit"
```

---

## Self-review notes

- **Spec coverage:** partition migration (T2), OTA client cluster (T9), block write/apply (T10), burst mode + stall watchdog (T11), rollback self-check (T12), version scheme (T3/T4), `.ota` build + index (T5/T6), GitHub Action (T8), z2m wiring (T7), rollout/docs (T13), end-to-end + rollback verification (T14). All spec sections mapped.
- **API risk:** isolated to T1 (confirm) and used in T9–T11; each firmware task notes "prefer the header if names differ."
- **Open detail flagged for execution:** exact OTA header byte offsets in `make_ota_image.py` (T5) are validated against z2m ingest in T14; fast-poll API name in T11 confirmed in T1.
