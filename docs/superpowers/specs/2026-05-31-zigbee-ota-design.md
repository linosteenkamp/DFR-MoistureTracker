# SP: Zigbee OTA Firmware Updates via zigbee2mqtt + GitHub

**Date:** 2026-05-31
**Status:** Design (approved sections 1–3; pending written-spec review)
**Target:** DFR-MoistureTracker, `dfrobot_firebeetle2_esp32c6_zigbee` env

## Problem

Deployed solar soil sensors can currently only be updated by physically
visiting each node with a USB cable (and opening its housing). This is a
"schlep" that does not scale as the fleet grows. We want **remote, fleet-wide
firmware updates** delivered over the existing Zigbee mesh through
zigbee2mqtt (z2m), with firmware binaries hosted on GitHub.

## Decisions (locked)

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Transport | **Zigbee OTA via z2m** (OTA Upgrade cluster 0x0019) | Fully remote; no site visits after bootstrap |
| Binary delivery | **GitHub Actions → GitHub Releases + auto-generated index** | Versioned, auditable, automated |
| Rollout | **Manual / staged per-device in z2m** | Canary one node before the fleet; a bad image can't brick everything at once |
| Recovery | **ESP-IDF app rollback only** | Auto-revert on failed self-check; staged rollout catches "boots-but-misbehaves" |
| Sleepy-ED download | **Burst mode (A):** rx-on + suspend light sleep + fast poll during OTA | Download in minutes not hours; transient power cost irrelevant on solar |

Explicitly **out of scope** (future hardening): SoftAP HTTP recovery,
automatic fleet rollout, secure boot / signed-app / anti-rollback, remote
"force-revert" command.

## Architecture overview

```
  git tag vX.Y.Z
        │
        ▼
  GitHub Actions ──build──> firmware.bin ──wrap──> firmware-vX.Y.Z.ota
        │                                                │
        ├── create Release, attach .ota asset ───────────┘
        └── regenerate index.json (stable raw URL)
                        │
                        ▼
                 zigbee2mqtt  (ota.zigbee_ota_override_index_location → index.json)
                        │  offers image when device fileVersion < index fileVersion
                        │  (manual: operator clicks "Update" per device)
                        ▼
                 Sensor (OTA Upgrade client)
                   burst-mode download → write inactive slot →
                   reboot (pending-verify) → rejoin + 1 good report →
                   mark valid  (else bootloader auto-reverts)
```

## On-device design

### Partition migration (one-time bootstrap)

Replace the single `factory` app with a dual-OTA layout on the 4 MB flash.

New `partitions.csv`:

| Name | Type | SubType | Size | Note |
|------|------|---------|------|------|
| nvs | data | nvs | 0x6000 | unchanged |
| phy_init | data | phy | 0x1000 | unchanged |
| otadata | data | ota | 0x2000 | new — tracks active slot |
| ota_0 | app | ota_0 | 0x180000 (1.5 MB) | app slot A (app ~1.2 MB → ~25% headroom) |
| ota_1 | app | ota_1 | 0x180000 (1.5 MB) | app slot B |
| storage | data | nvs | 0x4000 | unchanged |
| zb_storage | data | fat | 0x4000 | Zigbee pairing/NVRAM |
| zb_fct | data | fat | 0x400 | |

Total ≈ 3.3 MB of 4 MB (+ bootloader/partition-table region). Fits with
headroom.

**Consequence:** changing the partition table means the transition firmware
must be flashed **over USB once per node**, and the node **re-pairs** (zb_storage
offset shifts, wiping Zigbee NVRAM). This is the bootstrap cost; all subsequent
updates are wireless. It folds into the cable visit already needed.

sdkconfig additions (zigbee env): enable app rollback
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`); OTA partition layout is picked up
from `partitions.csv`. Anti-rollback and secure boot are **not** enabled (keeps
the ability to revert/downgrade).

### OTA Upgrade client (`ota_client` module)

- Register the **OTA Upgrade cluster (0x0019) as client** on the application
  endpoint, using esp-zigbee-sdk 1.6.x OTA APIs (backed by ESP-IDF `esp_ota_*`).
- Report current **`manufacturerCode`**, **`imageType`**, **`fileVersion`** in
  the query so z2m can decide whether to offer an image.
- Received image blocks are written to the **inactive OTA slot**
  (`esp_ota_get_next_update_partition` + `esp_ota_write`); on completion verify
  and set the boot partition; reboot.
- New module: `src/ota_client.c` / `include/ota_client.h`, registered in
  `src/CMakeLists.txt` and wired from the Zigbee init path. All behind
  `#ifdef USE_ZIGBEE`.

### Burst mode (approach A)

On OTA start (first image-block / OTA-start status):

1. Set `ota_active` flag.
2. **Pause the periodic report task** — no soil/ADC/e-paper work mid-transfer
   (also avoids the ADC-unit-rebuild churn during OTA).
3. **rx-on + suspend light sleep + fast poll (~250 ms)** so blocks stream
   quickly. Reuse the existing PM-lock / `esp_zb_sleep_enable` infrastructure.
4. **Stall watchdog** (~60 s with no block received) → abort, restore sleepy
   mode, so a failed transfer cannot hold the radio awake and drain a
   cloudy-day battery.

On success → reboot into the new slot. On abort/finish without reboot → restore
sleepy mode (rx-off, light sleep, resume reports).

### Rollback self-check

- A freshly-OTA'd image boots in **pending-verify**
  (`esp_ota_get_state_partition` == `ESP_OTA_IMG_PENDING_VERIFY`).
- Validation gate: the image must **rejoin Zigbee and push one good report**
  within a timeout (reuse `zigbee_reporter_wait_ready` + first report success).
  On success → `esp_ota_mark_app_valid_cancel_rollback()`.
- On failure → `esp_ota_mark_app_invalid_rollback_and_reboot()`; the bootloader
  reverts to the previous slot.

## Build & GitHub delivery

### Version scheme

Single source of truth = the **git tag `vX.Y.Z`**. CI packs it into the 32-bit
Zigbee `fileVersion` as `0xXXYYZZ00` (major/minor/patch bytes; low byte reserved
for a build counter). The firmware reports this same value as its current
version and surfaces `vX.Y.Z` on the e-paper / `label` path for eyeball
confirmation. z2m only offers an image with a **higher** `fileVersion` than the
node reports.

### Fixed product constants (one shared header + CI vars)

These must match in three places — firmware OTA config, the `.ota` header, and
the z2m index — so they live in one place and are referenced everywhere:

- `OTA_MANUFACTURER_CODE` = `0xFEFE` (chosen DIY 16-bit code)
- `OTA_IMAGE_TYPE` = `0x0001`
- `modelId` = `DFR-SoilSensor` (existing)

Defined in a shared header (e.g. `include/ota_ids.h`) consumed by the firmware,
and mirrored as environment/workflow variables in CI.

### The `.ota` image

A Zigbee OTA Upgrade file = standard OTA header (`manufacturerCode`,
`imageType`, `fileVersion`, total image size) wrapping a single upgrade
sub-element containing the ESP-IDF app `firmware.bin`. Produced by an image-wrap
step in CI (Espressif's Zigbee OTA image tooling or an equivalent script checked
into `tools/`).

### GitHub Action (release workflow)

New workflow `.github/workflows/release-ota.yml`, **triggered on tag push `v*`**:

```yaml
name: Release OTA
on:
  push:
    tags: ['v*']

permissions:
  contents: write          # create releases, commit index.json

env:
  OTA_MANUFACTURER_CODE: "0xFEFE"
  OTA_IMAGE_TYPE: "0x0001"
  MODEL_ID: "DFR-SoilSensor"

jobs:
  build-and-release:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Derive version from tag
        id: ver
        run: |
          TAG="${GITHUB_REF_NAME#v}"          # X.Y.Z
          IFS=. read -r MA MI PA <<< "$TAG"
          printf 'file_version=0x%02X%02X%02X00\n' "$MA" "$MI" "$PA" >> "$GITHUB_OUTPUT"
          echo "semver=$TAG" >> "$GITHUB_OUTPUT"

      - name: Set up PlatformIO
        uses: actions/setup-python@v5
        with: { python-version: '3.x' }
      - run: pip install platformio

      - name: Build Zigbee firmware
        run: |
          cp include/mqtt_credentials.h.example include/mqtt_credentials.h
          pio run -e dfrobot_firebeetle2_esp32c6_zigbee

      - name: Wrap firmware into .ota
        run: |
          python tools/make_ota_image.py \
            --in .pio/build/dfrobot_firebeetle2_esp32c6_zigbee/firmware.bin \
            --out firmware-${{ steps.ver.outputs.semver }}.ota \
            --manufacturer ${{ env.OTA_MANUFACTURER_CODE }} \
            --image-type ${{ env.OTA_IMAGE_TYPE }} \
            --file-version ${{ steps.ver.outputs.file_version }}

      - name: Create GitHub Release with .ota asset
        uses: softprops/action-gh-release@v2
        with:
          files: firmware-${{ steps.ver.outputs.semver }}.ota

      - name: Regenerate OTA index.json
        run: |
          python tools/update_ota_index.py \
            --index ota/index.json \
            --model "${{ env.MODEL_ID }}" \
            --manufacturer ${{ env.OTA_MANUFACTURER_CODE }} \
            --image-type ${{ env.OTA_IMAGE_TYPE }} \
            --file-version ${{ steps.ver.outputs.file_version }} \
            --url "https://github.com/${{ github.repository }}/releases/download/${{ github.ref_name }}/firmware-${{ steps.ver.outputs.semver }}.ota" \
            --image firmware-${{ steps.ver.outputs.semver }}.ota

      - name: Commit updated index to master
        run: |
          # The workflow runs from a detached tag checkout. Commit the index onto
          # the master branch tip explicitly so the raw URL always serves latest
          # and we never move master backward to the tagged commit.
          git config user.name  "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          git fetch origin master
          git checkout -B master origin/master
          # regenerate index against this checkout (re-run update step or move the
          # generated ota/index.json here), then:
          git add ota/index.json
          git commit -m "ota: index for ${{ github.ref_name }}" || echo "no change"
          git push origin master
```

Notes:
- `tools/make_ota_image.py` and `tools/update_ota_index.py` are small scripts
  added in this SP. The index entry records `modelId`, `manufacturerCode`,
  `imageType`, `fileVersion`, `url`, plus `imageSize`/`sha512` for integrity.
- **Index publication ordering** (resolve in the plan): the index must be
  regenerated against the `master` tip it is committed to (sequence the regenerate
  step after the `master` checkout, or carry the generated file across). The
  alternative — publishing `index.json` *also* as a Release asset at a fixed
  "latest" tag — avoids committing to `master` entirely and is worth weighing
  during planning.
- `index.json` is served at a **stable raw URL**
  (`https://raw.githubusercontent.com/<owner>/<repo>/master/ota/index.json`) — no
  GitHub Pages required. Committing it back to `master` keeps the latest index at
  that fixed location.
- The build needs `include/mqtt_credentials.h` to exist; the Zigbee build does
  not use MQTT but the file is included — CI seeds it from the example.

### z2m wiring

- Config: `ota.zigbee_ota_override_index_location: '<raw URL of ota/index.json>'`.
- `z2m/dfr_soil_moisture.js` gains an **`ota`** definition so z2m treats the
  device as updatable and matches it by `manufacturerName`/`modelID` + the fixed
  codes.
- **Staged rollout:** automatic update checks disabled; operator triggers per
  device in the z2m UI (device → OTA → Update).

## Rollout procedure (documented in DEVELOPER_GUIDE.md)

1. **Bootstrap (once per node, USB):** flash OTA-capable firmware (new partition
   table), re-pair in z2m. Wireless thereafter.
2. **Release:** `git tag vX.Y.Z && git push --tags` → CI builds, publishes the
   `.ota` Release asset, updates `index.json`.
3. **Canary:** z2m → one sensor → OTA → Update. Watch burst-mode download
   (~minutes) → reboot → rejoin → one good report → self-validate. Confirm
   soil/battery sane and `vX.Y.Z` shown.
4. **Fleet:** trigger Update on remaining nodes. Failed self-checks auto-revert.

## Testing & verification

- **Host tests:** version-packing helper (tag → `fileVersion` uint32) added to
  the `native` suite; `.ota` header construction unit-testable if the wrap logic
  is split into pure functions.
- **On-device (manual, per DEVELOPER_GUIDE checklist):** bootstrap flash + pair;
  publish a `vX.Y.Z+1`; canary update; confirm download, reboot, rejoin, report,
  and `esp_ota` slot switch; deliberately publish a known-bad image to confirm
  the rollback self-check reverts.
- **CI:** the workflow itself is exercised on the first real tag; a dry-run job
  (build + wrap + index regenerate, no Release) can validate on PRs.

## Risks / watch-items

- **esp-zigbee 1.6.x OTA API surface** — verify the exact client
  registration/callback API against the installed lib **first** (de-risk in the
  plan's opening task; the 1.6.x vs 2.x split already bit us once).
- **Flash budget** — 1.5 MB slots leave ~25% growth; re-balance if the app
  outgrows it.
- **Bootstrap re-pair** — unavoidable one-time cost; documented.
- **Interaction with ADC/light-sleep fixes** — benign by design: OTA pauses the
  report task, so no ADC reads occur mid-update.
- **Image authenticity** — relies on Zigbee network encryption + image CRC, not
  signed images. Acceptable for a private mesh; flagged for future hardening.

## Definition of done

- New OTA partition layout builds and flashes; device boots, joins, reports.
- A node updates end-to-end from a GitHub-published `.ota` via z2m (staged,
  manual), reboots into the new image, rejoins, and self-validates.
- A deliberately-bad image auto-reverts via rollback.
- CI publishes Release asset + `index.json` on a `v*` tag.
- `DEVELOPER_GUIDE.md` documents bootstrap, release, canary, fleet, and rollback.
