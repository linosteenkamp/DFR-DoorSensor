# Productionized Zigbee OTA Pipeline — Design Spec

**Date:** 2026-07-02
**Status:** Approved (design), pending implementation plan
**Repo:** DFR-DoorSensor (hosts the shared aggregator tool) + each `DFR-*` firmware repo
**z2m host:** Raspberry Pi, `ssh lino@499.steenkamps.org`, z2m running **natively** (not Docker)

## 1. Problem & Goal

Today, updating a device's firmware over Zigbee is manual: build the image, copy the
`.ota` to the Pi, run a throwaway web server, point z2m at it. The intended
GitHub-Actions pipeline exists in `DFR-MoistureTracker` (`release-ota.yml` +
`tools/make_ota_image.py` + `tools/update_ota_index.py`) but isn't wired up (its
`ota/index.json` is empty), and DFR-DoorSensor has none of it. Even fully wired, the
source repos are **private**, so z2m — which fetches the OTA index and each `.ota`
by plain URL with **no auth** — cannot pull directly from GitHub.

**Goal:** a hands-off pipeline where tagging a firmware release makes the new image
available to z2m automatically, with no manual copying or ad-hoc servers, keeping
source repos private, and covering the whole `DFR-*` device family through a single
z2m configuration.

## 2. Locked Decisions

- **Hosting:** LAN self-host, auto-synced. Artifacts published to **private** GitHub
  Releases; the Pi pulls them with a read-only token and re-serves on the LAN.
- **Transport:** **pull-based** — a systemd timer on the Pi polls Releases (~15 min).
  No inbound access to the Pi; decoupled from CI timing.
- **Host integration:** z2m is native → **systemd** units on the Pi.
- **Index scope:** **one combined family index** covering all `DFR-*` devices; z2m has
  a single `zigbee_ota_override_index_location`.
- **Repo set:** an **explicit list** of repos (adding a device = one config line). Not
  topic/glob auto-discovery (deferred).
- **Rollout:** **manual per-device** — `disable_automatic_update_check: true`; updates
  triggered in the z2m UI. A bad image never auto-deploys fleet-wide.
- **Tool location:** the aggregator (`dfr-ota-sync`) is a **standalone tool inside the
  DFR-DoorSensor repo** (family-scoped infra that happens to live here).
- **Serving:** `python3 -m http.server` on **:8091**, LAN-only, read-only dir.

## 3. Architecture

```
per-repo CI (each DFR-*, private)          Raspberry Pi (native z2m + systemd)
  git tag v* → build firmware              dfr-ota-sync.timer (~15 min)
   → make_ota_image.py (.ota)               └▶ dfr-ota-sync.service (oneshot)
   → update_ota_index.py (ota/index.json)       │ for each repo in repos.conf:
   → GitHub Release (.ota asset)  ──────────────┤   GH API: latest release + ota/index.json
   → commit ota/index.json                      │   download .ota asset (RO token)
                                                 │   verify sha512, rewrite url→LAN
                                                 │   merge entries
                                                 │ atomic swap → /var/lib/dfr-ota/
                                                 ▼
                                       dfr-ota-serve.service
                                       python3 -m http.server :8091 (LAN, RO)
                                                 │
                                                 ▼  http://<pi-lan>:8091/index.json
                                       zigbee2mqtt (override index; manual updates)
```

## 4. Components

### 4.1 Per-repo release CI (each firmware repo owns its artifacts)

DFR-DoorSensor gains the workflow it lacks, ported from MoistureTracker:
- `tools/make_ota_image.py`, `tools/update_ota_index.py`, `tools/test_ota_tools.py`
  (ported; door-sensor identity **manufacturer `0xFEFE`, image type `0x0002`**,
  model `DFR-DoorSensor`).
- `.github/workflows/release-ota.yml` on `push: tags: ['v*']`: derive version from
  tag → `pio test -e native` + build the zigbee env with `FW_VER_*` injected →
  `make_ota_image.py` → `update_ota_index.py` (url = the Release asset download URL) →
  publish Release with the `.ota` asset → commit `ota/index.json`. Drop MoistureTracker's
  soil-specific `mqtt_credentials.h` copy step; build env
  `dfrobot_firebeetle2_esp32c6_zigbee`.
- MoistureTracker's existing pipeline is verified/repaired so it too produces releases
  (its empty index means it has never tagged one).

The committed `ota/index.json` per repo is the **authoritative metadata** (modelId,
manufacturerCode, imageType, fileVersion, fileSize, sha512, url) the aggregator reuses.

### 4.2 `dfr-ota-sync` aggregator (the new core) — `DFR-DoorSensor/ota-server/`

Python 3 **stdlib only** (the Pi has python3; no pip deps). Files:
- `dfr_ota_sync.py` — the sync tool.
- `repos.conf.example` — sample config.
- `systemd/dfr-ota-sync.service`, `systemd/dfr-ota-sync.timer`, `systemd/dfr-ota-serve.service`.
- `tests/test_dfr_ota_sync.py` — pytest for the pure logic.
- `README.md` — install + operation.

**Config** (`/etc/dfr-ota/repos.conf`): one `owner/repo` per line (comments with `#`).
**Secret** (`/etc/dfr-ota/token`, `chmod 600`): a fine-grained GitHub PAT, **Contents:
read** on exactly the listed repos.

**Sync algorithm** (per run):
1. For each repo in `repos.conf`:
   a. GitHub API `GET /repos/{repo}/releases/latest` (Bearer token).
   b. Fetch the committed `ota/index.json` (API contents, raw) → the entry/entries.
   c. For each entry, find the matching release **asset** by the filename (basename of
      the entry `url`); download it via the asset API
      (`GET /releases/assets/{id}`, `Accept: application/octet-stream`) into a staging dir.
   d. Verify `sha512(downloaded) == entry.sha512` and `size == entry.fileSize`; on
      mismatch, fail this repo (skip it, keep last-good), log, continue others.
   e. Rewrite `entry.url` → `http://<pi-lan>:8091/ota/<filename>`.
2. Merge all repos' verified entries into one `index.json`.
3. **Atomic swap**: write files + index into a staging dir, then atomically move into
   `/var/lib/dfr-ota/` (rename), so z2m never observes a partial state.
4. Exit non-zero if any repo failed (timer/journal surface it), but still publish the
   entries that succeeded (a broken repo never removes healthy images).

A repo with no release yet, or no `ota/index.json`, is **skipped** (not an error).

### 4.3 Serving

`dfr-ota-serve.service`: `python3 -m http.server 8091 --bind <lan-ip> --directory
/var/lib/dfr-ota`, run as the `dfr-ota` user, read-only content.

### 4.4 z2m configuration

In z2m `configuration.yaml`:
```yaml
ota:
  zigbee_ota_override_index_location: http://<pi-lan>:8091/index.json
  disable_automatic_update_check: true
```
Updates are triggered per device in the z2m UI (Device → OTA → Update).

## 5. Index entry schema (z2m)

Unchanged from `update_ota_index.py`; the merged index is the concatenation of all
repos' entries with `url` rewritten:
```json
{ "modelId": "DFR-DoorSensor", "manufacturerCode": 65278, "imageType": 2,
  "fileVersion": 16777472, "fileSize": 12345,
  "sha512": "…", "url": "http://<pi-lan>:8091/ota/firmware-1.0.1.ota" }
// manufacturerCode 65278 = 0xFEFE, imageType 2 = 0x0002,
// fileVersion 16777472 = 0x01000100 (v1.0.1, packed major<<24|minor<<16|patch<<8|build)
```
z2m offers an update when a matching entry's `fileVersion` exceeds the device's running
version (devices report `FW_VERSION_U32`).

## 6. Deployment

Dedicated unprivileged user `dfr-ota`. `install.sh` (or documented steps): create user +
`/var/lib/dfr-ota` + `/etc/dfr-ota/{repos.conf,token}`; install the three systemd units;
`systemctl enable --now dfr-ota-sync.timer dfr-ota-serve.service`. Manual refresh:
`systemctl start dfr-ota-sync.service`. Logs via journald.

## 7. Failure handling & security

- **Integrity:** sha512 + size verified on every download before an entry is served.
- **Atomicity:** staging + rename; last-good serve dir retained on partial failure.
- **Least privilege:** token is read-only, scoped to the listed repos, `chmod 600`,
  never leaves the Pi. Services run as `dfr-ota` (no root).
- **Exposure:** static server is LAN-only and read-only. Source stays private.
- **Observability:** sync exits non-zero on any repo failure; journald logs per-repo
  outcome and version.

## 8. Testing

- **Host (pytest), pure logic:** entry merge across repos, `url` rewrite, sha512/size
  verification decision, atomic-swap decision, `repos.conf` parsing. Mirrors
  `tools/test_ota_tools.py`. GitHub API calls isolated behind a thin fetch function so
  tests use recorded fixtures (no network).
- **Integration (manual/CI):** run `dfr_ota_sync.py` once against the real private
  Releases with a token; assert a valid merged `index.json` + a downloaded, sha512-valid
  `.ota`.
- **On-Pi acceptance:** `curl http://<pi>:8091/index.json`; z2m "check for updates"
  shows the new version for a device; trigger one device update end-to-end and confirm
  it applies and reboots onto the new `fileVersion`.

## 9. Out of scope / future

- Topic/glob auto-discovery of `DFR-*` repos (explicit list for now).
- Push-based delivery / self-hosted GitHub runner.
- Public artifact mirror.
- Fleet dashboards, staged % rollouts, auto-rollback orchestration (device already has
  bootloader rollback).
- Migrating `dfr-ota-sync` into the future shared `esp-zb-common` infrastructure.

## 10. Confirm at implementation (Pi currently unreachable from dev env)

- Exact z2m `configuration.yaml` path and the running z2m version's OTA config keys
  (`ota.zigbee_ota_override_index_location` / `disable_automatic_update_check`).
- The Pi's LAN IP/hostname to bind/serve and put in the z2m override URL.
- `python3` version present; confirm stdlib-only is sufficient.
