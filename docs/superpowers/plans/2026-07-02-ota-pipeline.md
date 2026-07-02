# Productionized OTA Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tagging a firmware release makes the new image available to Zigbee2MQTT automatically — no manual copying or ad-hoc web servers — across the whole `DFR-*` family, with source repos staying private.

**Architecture:** Each firmware repo's CI publishes a `.ota` to a **private** GitHub Release and commits its `ota/index.json`. A pull-based aggregator on the Pi (`dfr-ota-server`, new dedicated repo) polls those Releases with a read-only token every ~15 min, verifies each image's sha512, rewrites URLs to a LAN address, merges everything into one index, and atomically publishes it; a tiny static server serves it on `:8091`. z2m uses it as an override index with manual per-device rollout.

**Tech Stack:** Python 3 stdlib only (aggregator), pytest (dev/CI), systemd, GitHub Actions, PlatformIO/ESP-IDF (firmware build in per-repo CI).

## Global Constraints

- **Aggregator language:** Python 3 **stdlib only** — no pip runtime deps. pytest is dev/CI only.
- **Aggregator repo:** new private repo `dfr-ota-server`; local working dir `/Users/lino/Developer/499/dfr-ota-server`.
- **Serve:** `python3 -m http.server 8091`, LAN-only, read-only; serve dir `/var/lib/dfr-ota` containing `index.json` + `ota/*.ota`.
- **z2m override URL:** `http://<pi-lan>:8091/index.json`; `ota.disable_automatic_update_check: true`.
- **Door-sensor OTA identity (exact):** manufacturer `0xFEFE`, image type `0x0002`, model `DFR-DoorSensor`, header string `DFR-DoorSensor OTA`.
- **Index entry schema (z2m):** `modelId, manufacturerCode (int), imageType (int), fileVersion (int), fileSize (int), sha512 (hex str), url`.
- **Entry identity for merge/dedupe:** `(manufacturerCode, imageType)`; newer overwrites.
- **Version packing:** `fileVersion = (major<<24)|(minor<<16)|(patch<<8)|build`.
- **Secrets:** read-only fine-grained GitHub PAT (Contents:read on the listed repos) at `/etc/dfr-ota/token` (chmod 600); repo list at `/etc/dfr-ota/repos.conf`. Never commit either.
- **Private-repo asset download:** GitHub asset API 302-redirects to S3 which rejects the `Authorization` header — the fetcher MUST strip `Authorization` on redirect.
- **Atomicity:** build in staging, verify all, then swap; a failing repo never removes healthy images (last-good retained).
- **Pi env facts to confirm at deploy (Pi unreachable from dev now):** exact z2m `configuration.yaml` path, running z2m OTA config keys, Pi LAN IP, `python3` version.

---

## File Structure

**`dfr-ota-server`** (new repo):
- `dfr_ota_sync.py` — aggregator: pure helpers + `sync_once` orchestrator + real `GitHubFetcher` + `main()`.
- `repos.conf.example` — sample repo list.
- `systemd/dfr-ota-sync.service`, `systemd/dfr-ota-sync.timer`, `systemd/dfr-ota-serve.service`.
- `install.sh` — user/dirs/units bootstrap.
- `tests/test_dfr_ota_sync.py` — pytest (pure helpers + `sync_once` via a fake fetcher).
- `.github/workflows/ci.yml` — run pytest on push.
- `README.md` — install/operation + z2m snippet.

**`DFR-DoorSensor`** (existing repo):
- `tools/make_ota_image.py`, `tools/update_ota_index.py`, `tools/test_ota_tools.py` — ported.
- `ota/index.json` — seeded `[]`.
- `.github/workflows/release-ota.yml` — tag-triggered release.

---

## Phase A — `dfr-ota-server` aggregator

### Task A1: Scaffold repo + pytest/CI harness + `repos.conf` parsing (TDD)

**Files:**
- Create: `/Users/lino/Developer/499/dfr-ota-server/{dfr_ota_sync.py, tests/test_dfr_ota_sync.py, repos.conf.example, .github/workflows/ci.yml, .gitignore, README.md}`

**Interfaces:**
- Consumes: nothing.
- Produces: `parse_repos_conf(text: str) -> list[str]` (skips blank/`#` lines, dedupes preserving order, raises `ValueError` on a malformed non-comment line).

- [ ] **Step 1: Init the repo and layout**

```bash
mkdir -p /Users/lino/Developer/499/dfr-ota-server/{tests,systemd,.github/workflows}
cd /Users/lino/Developer/499/dfr-ota-server
git init -q && git branch -m main
printf '__pycache__/\n*.pyc\n/.staging/\n' > .gitignore
printf '# One owner/repo per line. Blank lines and # comments ignored.\nlinosteenkamp/DFR-DoorSensor\n' > repos.conf.example
```

- [ ] **Step 2: Write the failing test — `tests/test_dfr_ota_sync.py`**

```python
import sys
from pathlib import Path
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import dfr_ota_sync as s


def test_parse_repos_conf_basic():
    text = "# comment\n\nlinosteenkamp/DFR-DoorSensor\n  owner/repo2  \n"
    assert s.parse_repos_conf(text) == ["linosteenkamp/DFR-DoorSensor", "owner/repo2"]


def test_parse_repos_conf_dedupes_preserving_order():
    text = "a/b\nc/d\na/b\n"
    assert s.parse_repos_conf(text) == ["a/b", "c/d"]


def test_parse_repos_conf_rejects_malformed():
    with pytest.raises(ValueError):
        s.parse_repos_conf("not-a-repo\n")
    with pytest.raises(ValueError):
        s.parse_repos_conf("owner/repo extra\n")
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cd /Users/lino/Developer/499/dfr-ota-server && python3 -m pytest tests/ -q`
Expected: FAIL — `AttributeError: module 'dfr_ota_sync' has no attribute 'parse_repos_conf'` (or import error for the empty module).

- [ ] **Step 4: Create `dfr_ota_sync.py` with the parser**

```python
#!/usr/bin/env python3
"""dfr-ota-server: aggregate per-repo Zigbee OTA releases into one LAN-served z2m index."""
from __future__ import annotations


def parse_repos_conf(text: str) -> list[str]:
    """Parse repos.conf into an ordered, de-duplicated list of 'owner/repo'."""
    repos: list[str] = []
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if " " in s or s.count("/") != 1 or s.startswith("/") or s.endswith("/"):
            raise ValueError(f"malformed repo line: {line!r}")
        if s not in repos:
            repos.append(s)
    return repos
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `python3 -m pytest tests/ -q`
Expected: PASS — 3 passed.

- [ ] **Step 6: Add CI workflow `.github/workflows/ci.yml`**

```yaml
name: CI
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: '3.x'
      - run: pip install pytest
      - run: python3 -m pytest tests/ -q
```

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "chore: scaffold dfr-ota-server + repos.conf parser (TDD)"
```

---

### Task A2: `rewrite_entry_url` (TDD)

**Files:**
- Modify: `dfr_ota_sync.py`, `tests/test_dfr_ota_sync.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `rewrite_entry_url(entry: dict, base_url: str) -> dict` — returns a **new** dict with `url` replaced by `{base_url}/ota/{basename(original url)}`; other keys unchanged.

- [ ] **Step 1: Add the failing test**

```python
def test_rewrite_entry_url():
    entry = {"modelId": "X", "manufacturerCode": 65278, "imageType": 2,
             "fileVersion": 1, "fileSize": 10, "sha512": "ab",
             "url": "https://github.com/o/r/releases/download/v1.0.1/firmware-1.0.1.ota"}
    out = s.rewrite_entry_url(entry, "http://pi.lan:8091")
    assert out["url"] == "http://pi.lan:8091/ota/firmware-1.0.1.ota"
    assert out["sha512"] == "ab" and out["manufacturerCode"] == 65278
    assert entry["url"].startswith("https://")  # original not mutated


def test_rewrite_entry_url_strips_trailing_slash_on_base():
    entry = {"url": "https://x/y/fw.ota"}
    assert s.rewrite_entry_url(entry, "http://pi.lan:8091/")["url"] == "http://pi.lan:8091/ota/fw.ota"
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 -m pytest tests/ -q -k rewrite`
Expected: FAIL — `AttributeError: ... 'rewrite_entry_url'`.

- [ ] **Step 3: Implement in `dfr_ota_sync.py`**

```python
def rewrite_entry_url(entry: dict, base_url: str) -> dict:
    """Return a copy of entry with url repointed to the LAN server."""
    fname = entry["url"].rstrip("/").rsplit("/", 1)[-1]
    return {**entry, "url": f"{base_url.rstrip('/')}/ota/{fname}"}
```

- [ ] **Step 4: Run to verify it passes**

Run: `python3 -m pytest tests/ -q -k rewrite`
Expected: PASS — 2 passed.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat: rewrite_entry_url (LAN url rewrite)"
```

---

### Task A3: `verify_blob` (TDD)

**Files:**
- Modify: `dfr_ota_sync.py`, `tests/test_dfr_ota_sync.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `class VerifyError(Exception)`; `verify_blob(blob: bytes, expected_sha512: str, expected_size: int) -> None` — raises `VerifyError` on size or sha512 mismatch, else returns None.

- [ ] **Step 1: Add the failing test**

```python
import hashlib

def test_verify_blob_ok():
    blob = b"hello world"
    s.verify_blob(blob, hashlib.sha512(blob).hexdigest(), len(blob))  # no raise

def test_verify_blob_size_mismatch():
    blob = b"abc"
    with pytest.raises(s.VerifyError):
        s.verify_blob(blob, hashlib.sha512(blob).hexdigest(), 999)

def test_verify_blob_sha_mismatch():
    with pytest.raises(s.VerifyError):
        s.verify_blob(b"abc", "00" * 64, 3)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 -m pytest tests/ -q -k verify_blob`
Expected: FAIL — `AttributeError: ... 'VerifyError'`.

- [ ] **Step 3: Implement in `dfr_ota_sync.py`**

```python
import hashlib


class VerifyError(Exception):
    pass


def verify_blob(blob: bytes, expected_sha512: str, expected_size: int) -> None:
    if len(blob) != expected_size:
        raise VerifyError(f"size {len(blob)} != expected {expected_size}")
    got = hashlib.sha512(blob).hexdigest()
    if got.lower() != expected_sha512.lower():
        raise VerifyError("sha512 mismatch")
```

- [ ] **Step 4: Run to verify it passes**

Run: `python3 -m pytest tests/ -q -k verify_blob`
Expected: PASS — 3 passed.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat: verify_blob (sha512 + size integrity)"
```

---

### Task A4: `merge_indexes` (TDD)

**Files:**
- Modify: `dfr_ota_sync.py`, `tests/test_dfr_ota_sync.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `merge_indexes(previous: list[dict], updates: list[dict]) -> list[dict]` — keys entries by `(manufacturerCode, imageType)`; `updates` overwrite `previous`; unaffected `previous` keys are retained (this is the "failed repo keeps last-good" carry-over). Returns a list.

- [ ] **Step 1: Add the failing test**

```python
def _e(mfg, itype, ver, url="https://x/y/f.ota"):
    return {"manufacturerCode": mfg, "imageType": itype, "fileVersion": ver, "url": url}

def test_merge_updates_overwrite_by_key():
    prev = [_e(65278, 1, 100), _e(65278, 2, 100)]
    upd = [_e(65278, 2, 101)]                       # newer door-sensor image
    out = s.merge_indexes(prev, upd)
    by = {(e["manufacturerCode"], e["imageType"]): e for e in out}
    assert by[(65278, 1)]["fileVersion"] == 100     # untouched repo retained
    assert by[(65278, 2)]["fileVersion"] == 101     # overwritten
    assert len(out) == 2

def test_merge_adds_new_device():
    out = s.merge_indexes([_e(65278, 1, 100)], [_e(65278, 9, 1)])
    assert len(out) == 2
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 -m pytest tests/ -q -k merge`
Expected: FAIL — `AttributeError: ... 'merge_indexes'`.

- [ ] **Step 3: Implement in `dfr_ota_sync.py`**

```python
def _key(entry: dict) -> tuple:
    return (entry["manufacturerCode"], entry["imageType"])


def merge_indexes(previous: list[dict], updates: list[dict]) -> list[dict]:
    """Merge by (manufacturerCode, imageType); updates overwrite, others retained."""
    merged = {_key(e): e for e in previous}
    for e in updates:
        merged[_key(e)] = e
    return list(merged.values())
```

- [ ] **Step 4: Run to verify it passes**

Run: `python3 -m pytest tests/ -q -k merge`
Expected: PASS — 2 passed.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat: merge_indexes (family merge, carry-over last-good)"
```

---

### Task A5: `sync_once` orchestrator + atomic publish (TDD with fake fetcher)

**Files:**
- Modify: `dfr_ota_sync.py`, `tests/test_dfr_ota_sync.py`

**Interfaces:**
- Consumes: `parse_repos_conf`, `rewrite_entry_url`, `verify_blob`, `merge_indexes`.
- Produces:
  - `class SkipRepo(Exception)` — raised by a fetcher's `get_index` when the repo has no release/index.
  - `sync_once(fetcher, repos: list[str], base_url: str, serve_dir, staging_root, log=print) -> dict` — for each repo calls `fetcher.get_index(repo) -> list[dict]` and `fetcher.download(repo, filename) -> bytes`; verifies each blob; on a fully-successful repo, stages its images and rewritten entries; on any repo failure, that repo is skipped and its previously-served entry/image is retained. Publishes atomically to `serve_dir` (`index.json` + `ota/`). Returns `{"ok": <int entries served>, "failed": [<repos>]}`.

- [ ] **Step 1: Add the failing test (fake fetcher, tmp dirs)**

```python
import json, hashlib
from pathlib import Path

def _entry(mfg, itype, ver, blob, fname):
    return {"modelId": f"m{itype}", "manufacturerCode": mfg, "imageType": itype,
            "fileVersion": ver, "fileSize": len(blob),
            "sha512": hashlib.sha512(blob).hexdigest(),
            "url": f"https://github.com/o/r/releases/download/v/{fname}"}

class FakeFetcher:
    def __init__(self, data):
        self.data = data  # {repo: {"entries":[...], "blobs":{fname: bytes}}}
    def get_index(self, repo):
        if repo not in self.data:
            raise s.SkipRepo(repo)
        return self.data[repo]["entries"]
    def download(self, repo, filename):
        return self.data[repo]["blobs"][filename]

def test_sync_once_publishes_valid_index(tmp_path):
    b = b"\x01" * 50
    data = {"o/r1": {"entries": [_entry(65278, 2, 101, b, "door.ota")],
                      "blobs": {"door.ota": b}}}
    serve = tmp_path / "serve"
    res = s.sync_once(FakeFetcher(data), ["o/r1"], "http://pi.lan:8091",
                      serve, tmp_path / "stg", log=lambda *a: None)
    assert res == {"ok": 1, "failed": []}
    idx = json.loads((serve / "index.json").read_text())
    assert idx[0]["url"] == "http://pi.lan:8091/ota/door.ota"
    assert (serve / "ota" / "door.ota").read_bytes() == b

def test_sync_once_keeps_last_good_when_repo_fails(tmp_path):
    b = b"\x02" * 40
    good = {"o/r1": {"entries": [_entry(65278, 2, 101, b, "door.ota")],
                     "blobs": {"door.ota": b}}}
    serve = tmp_path / "serve"
    s.sync_once(FakeFetcher(good), ["o/r1"], "http://pi.lan:8091",
                serve, tmp_path / "stg", log=lambda *a: None)
    # Next run: same repo now returns a corrupted blob (sha mismatch) -> repo fails
    bad = {"o/r1": {"entries": [_entry(65278, 2, 102, b, "door.ota")],
                    "blobs": {"door.ota": b"\xFF" * 40}}}
    res = s.sync_once(FakeFetcher(bad), ["o/r1"], "http://pi.lan:8091",
                      serve, tmp_path / "stg", log=lambda *a: None)
    assert res["failed"] == ["o/r1"]
    idx = json.loads((serve / "index.json").read_text())
    assert idx[0]["fileVersion"] == 101                 # last-good retained
    assert (serve / "ota" / "door.ota").read_bytes() == b

def test_sync_once_skips_repo_without_release(tmp_path):
    serve = tmp_path / "serve"
    res = s.sync_once(FakeFetcher({}), ["o/none"], "http://pi.lan:8091",
                      serve, tmp_path / "stg", log=lambda *a: None)
    assert res == {"ok": 0, "failed": []}
    assert json.loads((serve / "index.json").read_text()) == []
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 -m pytest tests/ -q -k sync_once`
Expected: FAIL — `AttributeError: ... 'sync_once'` / `'SkipRepo'`.

- [ ] **Step 3: Implement in `dfr_ota_sync.py`**

```python
import json
import os
import shutil
import tempfile
from pathlib import Path


class SkipRepo(Exception):
    pass


def _load_index(path: Path) -> list[dict]:
    if path.is_file() and path.read_text().strip():
        return json.loads(path.read_text())
    return []


def _atomic_replace_dir(new_dir: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    backup = target.with_name(target.name + ".old")
    if backup.exists():
        shutil.rmtree(backup)
    if target.exists():
        os.rename(target, backup)
    os.rename(new_dir, target)
    if backup.exists():
        shutil.rmtree(backup)


def _publish(serve_dir: Path, staging_root: Path, merged: list[dict],
             new_blobs: dict) -> None:
    staging_root.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(dir=staging_root))
    ota = stage / "ota"
    ota.mkdir()
    src_ota = serve_dir / "ota"
    if src_ota.is_dir():                       # carry over last-good files
        for f in src_ota.iterdir():
            if f.is_file():
                shutil.copy2(f, ota / f.name)
    for fname, blob in new_blobs.items():      # overlay fresh images
        (ota / fname).write_bytes(blob)
    referenced = {e["url"].rstrip("/").rsplit("/", 1)[-1] for e in merged}
    for f in ota.iterdir():                    # prune orphans
        if f.is_file() and f.name not in referenced:
            f.unlink()
    (stage / "index.json").write_text(json.dumps(merged, indent=2) + "\n")
    _atomic_replace_dir(stage, serve_dir)


def sync_once(fetcher, repos, base_url, serve_dir, staging_root, log=print) -> dict:
    serve_dir = Path(serve_dir)
    staging_root = Path(staging_root)
    previous = _load_index(serve_dir / "index.json")
    updates: list[dict] = []
    new_blobs: dict = {}
    failed: list[str] = []

    for repo in repos:
        try:
            entries = fetcher.get_index(repo)
        except SkipRepo:
            log(f"{repo}: no release/index — skipping")
            continue
        except Exception as ex:                # noqa: BLE001 - report & continue
            log(f"{repo}: index fetch failed: {ex}")
            failed.append(repo)
            continue

        repo_updates: list[dict] = []
        repo_blobs: dict = {}
        ok = True
        for entry in entries:
            fname = entry["url"].rstrip("/").rsplit("/", 1)[-1]
            try:
                blob = fetcher.download(repo, fname)
                verify_blob(blob, entry["sha512"], entry["fileSize"])
            except Exception as ex:            # noqa: BLE001
                log(f"{repo}: {fname} failed: {ex}")
                ok = False
                break
            repo_blobs[fname] = blob
            repo_updates.append(rewrite_entry_url(entry, base_url))
        if ok:
            updates.extend(repo_updates)
            new_blobs.update(repo_blobs)
            log(f"{repo}: {len(repo_updates)} image(s) OK")
        else:
            failed.append(repo)

    merged = merge_indexes(previous, updates)
    _publish(serve_dir, staging_root, merged, new_blobs)
    return {"ok": len(merged), "failed": failed}
```

- [ ] **Step 4: Run to verify it passes**

Run: `python3 -m pytest tests/ -q`
Expected: PASS — all tests (parse/rewrite/verify/merge/sync) green.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat: sync_once orchestrator with atomic publish + last-good retention"
```

---

### Task A6: Real `GitHubFetcher` + `main()` + systemd units + install + README

**Files:**
- Modify: `dfr_ota_sync.py`
- Create: `systemd/dfr-ota-sync.service`, `systemd/dfr-ota-sync.timer`, `systemd/dfr-ota-serve.service`, `install.sh`, `README.md`

**Interfaces:**
- Consumes: `sync_once`, `parse_repos_conf`, `SkipRepo`.
- Produces: `GitHubFetcher(token)` with `.get_index(repo)` / `.download(repo, filename)`; `main(argv=None)` CLI. Not unit-tested (network); verified by the integration step and Phase C.

- [ ] **Step 1: Add `GitHubFetcher` + `main()` to `dfr_ota_sync.py`**

```python
import argparse
import urllib.error
import urllib.request

GITHUB_API = "https://api.github.com"


class _NoAuthRedirect(urllib.request.HTTPRedirectHandler):
    """Strip Authorization on redirect (GitHub asset -> S3 rejects the header)."""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None:
            new.headers.pop("Authorization", None)
            new.unredirected_hdrs.pop("Authorization", None)
        return new


class GitHubFetcher:
    def __init__(self, token: str):
        self._token = token
        self._opener = urllib.request.build_opener(_NoAuthRedirect())

    def _get(self, path: str, accept: str = "application/vnd.github+json") -> bytes:
        req = urllib.request.Request(
            GITHUB_API + path,
            headers={
                "Authorization": f"Bearer {self._token}",
                "Accept": accept,
                "X-GitHub-Api-Version": "2022-11-28",
                "User-Agent": "dfr-ota-sync",
            },
        )
        with self._opener.open(req, timeout=30) as resp:
            return resp.read()

    def get_index(self, repo: str) -> list[dict]:
        try:
            self._get(f"/repos/{repo}/releases/latest")
        except urllib.error.HTTPError as e:
            if e.code == 404:
                raise SkipRepo(repo)
            raise
        try:
            raw = self._get(f"/repos/{repo}/contents/ota/index.json",
                            accept="application/vnd.github.raw")
        except urllib.error.HTTPError as e:
            if e.code == 404:
                raise SkipRepo(repo)
            raise
        return json.loads(raw)

    def download(self, repo: str, filename: str) -> bytes:
        rel = json.loads(self._get(f"/repos/{repo}/releases/latest"))
        asset_id = next((a["id"] for a in rel.get("assets", [])
                         if a["name"] == filename), None)
        if asset_id is None:
            raise FileNotFoundError(f"{repo}: asset {filename} not in latest release")
        return self._get(f"/repos/{repo}/releases/assets/{asset_id}",
                         accept="application/octet-stream")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Aggregate DFR-* OTA releases for z2m.")
    ap.add_argument("--repos", default="/etc/dfr-ota/repos.conf")
    ap.add_argument("--token-file", default="/etc/dfr-ota/token")
    ap.add_argument("--serve-dir", default="/var/lib/dfr-ota")
    ap.add_argument("--staging-root", default="/var/lib/dfr-ota/.staging")
    ap.add_argument("--base-url", required=True,
                    help="LAN base, e.g. http://192.168.1.10:8091")
    a = ap.parse_args(argv)

    repos = parse_repos_conf(Path(a.repos).read_text())
    token = Path(a.token_file).read_text().strip()
    res = sync_once(GitHubFetcher(token), repos, a.base_url,
                    a.serve_dir, a.staging_root)
    print(f"served {res['ok']} image(s); failed repos: {res['failed'] or 'none'}")
    return 1 if res["failed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Create the systemd units**

`systemd/dfr-ota-sync.service`:
```ini
[Unit]
Description=DFR OTA index sync (pull GitHub Releases -> LAN index)
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
User=dfr-ota
# BASE_URL is set at install time (Pi LAN ip:port)
EnvironmentFile=/etc/dfr-ota/env
ExecStart=/usr/bin/python3 /opt/dfr-ota-server/dfr_ota_sync.py --base-url ${BASE_URL}
```

`systemd/dfr-ota-sync.timer`:
```ini
[Unit]
Description=Run DFR OTA sync every 15 minutes

[Timer]
OnBootSec=2min
OnUnitActiveSec=15min
Persistent=true

[Install]
WantedBy=timers.target
```

`systemd/dfr-ota-serve.service`:
```ini
[Unit]
Description=DFR OTA static server (LAN)
After=network-online.target

[Service]
User=dfr-ota
EnvironmentFile=/etc/dfr-ota/env
ExecStart=/usr/bin/python3 -m http.server ${SERVE_PORT} --bind ${BIND_ADDR} --directory /var/lib/dfr-ota
Restart=always

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 3: Create `install.sh`**

```bash
#!/usr/bin/env bash
# Install dfr-ota-server on the z2m Pi. Run as root. Idempotent.
set -euo pipefail
REPO_DIR="/opt/dfr-ota-server"
id dfr-ota &>/dev/null || useradd --system --home /var/lib/dfr-ota --shell /usr/sbin/nologin dfr-ota
install -d -o dfr-ota -g dfr-ota /var/lib/dfr-ota /var/lib/dfr-ota/.staging /var/lib/dfr-ota/ota
install -d -m 750 /etc/dfr-ota
[ -f /etc/dfr-ota/repos.conf ] || install -m 640 "$(dirname "$0")/repos.conf.example" /etc/dfr-ota/repos.conf
[ -f /etc/dfr-ota/token ] || { touch /etc/dfr-ota/token; chmod 600 /etc/dfr-ota/token; echo "!! put a read-only GitHub PAT in /etc/dfr-ota/token"; }
# /etc/dfr-ota/env holds BASE_URL, BIND_ADDR, SERVE_PORT (edit after install)
[ -f /etc/dfr-ota/env ] || cat > /etc/dfr-ota/env <<'EOF'
BASE_URL=http://REPLACE_WITH_PI_LAN_IP:8091
BIND_ADDR=0.0.0.0
SERVE_PORT=8091
EOF
mkdir -p "$REPO_DIR" && cp -r "$(dirname "$0")"/dfr_ota_sync.py "$REPO_DIR"/
cp systemd/dfr-ota-*.{service,timer} /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now dfr-ota-serve.service dfr-ota-sync.timer
echo "Installed. Set the token + edit /etc/dfr-ota/env, then: systemctl start dfr-ota-sync.service"
```

- [ ] **Step 4: Write `README.md`** (install, token scope, z2m snippet)

```markdown
# dfr-ota-server

Pull-based Zigbee OTA aggregator for a private DFR-* device family. A systemd
timer fetches each configured repo's latest GitHub Release `.ota` + `ota/index.json`
(read-only token), verifies sha512, and serves one merged z2m override index on the LAN.

## Install (on the z2m Pi)
1. `git clone` this repo; `sudo ./install.sh`.
2. Put a fine-grained GitHub PAT (Contents: read on the DFR-* repos) in `/etc/dfr-ota/token`.
3. Edit `/etc/dfr-ota/repos.conf` (one `owner/repo` per line) and `/etc/dfr-ota/env`
   (`BASE_URL=http://<pi-lan-ip>:8091`).
4. `sudo systemctl start dfr-ota-sync.service` then `curl http://<pi>:8091/index.json`.

## Zigbee2MQTT
```yaml
ota:
  zigbee_ota_override_index_location: http://<pi-lan-ip>:8091/index.json
  disable_automatic_update_check: true
```
Trigger updates per device in the z2m UI. Runs `--base-url` from `/etc/dfr-ota/env`.
```

- [ ] **Step 5: Sanity-check the module imports and CLI parses**

Run: `cd /Users/lino/Developer/499/dfr-ota-server && python3 -c "import dfr_ota_sync" && python3 dfr_ota_sync.py --base-url http://x:8091 --repos repos.conf.example --token-file /dev/null --serve-dir /tmp/sd --staging-root /tmp/sd/.stg; echo "exit=$?"`
Expected: runs; since `/dev/null` token → GitHub calls 401 → each repo reported failed (exit 1) OR skipped; no Python traceback. (Full success needs a real token — Phase C.)

- [ ] **Step 6: Run the full test suite (unchanged logic still green)**

Run: `python3 -m pytest tests/ -q`
Expected: PASS — all previous tests still pass.

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "feat: GitHubFetcher + CLI + systemd units + install + README"
```

---

## Phase B — DFR-DoorSensor release CI

Work in `/Users/lino/Developer/499/DFR-DoorSensor` on a branch off `main`.

### Task B1: Port OTA tools + seed index (TDD)

**Files:**
- Create: `tools/make_ota_image.py`, `tools/update_ota_index.py`, `tools/test_ota_tools.py`, `ota/index.json`

**Interfaces:**
- Produces: `make_ota_image.build(...)` and the two CLIs, identical to MoistureTracker's, driven by args for the door identity.

- [ ] **Step 1: Copy the three tools verbatim from MoistureTracker**

```bash
cd /Users/lino/Developer/499/DFR-DoorSensor
mkdir -p tools ota
cp ../DFR-MoistureTracker/tools/make_ota_image.py tools/
cp ../DFR-MoistureTracker/tools/update_ota_index.py tools/
cp ../DFR-MoistureTracker/tools/test_ota_tools.py tools/
printf '[]\n' > ota/index.json
```

- [ ] **Step 2: Run the ported tests to confirm they pass as-is**

Run: `cd /Users/lino/Developer/499/DFR-DoorSensor && python3 -m pytest tools/test_ota_tools.py -q`
Expected: PASS — 2 passed. (The tests assert the OTA header format and index replacement; identity values are passed as args, so they pass unchanged.)

- [ ] **Step 3: Commit**

```bash
git add tools/make_ota_image.py tools/update_ota_index.py tools/test_ota_tools.py ota/index.json
git commit -m "chore(ota): port OTA image + index tools; seed empty index"
```

---

### Task B2: Release workflow

**Files:**
- Create: `.github/workflows/release-ota.yml`

**Interfaces:**
- Consumes: `tools/make_ota_image.py`, `tools/update_ota_index.py`. Produces a GitHub Release + committed `ota/index.json` on a `v*` tag.

- [ ] **Step 1: Create `.github/workflows/release-ota.yml`** (ported; door identity, `main` branch, no mqtt step, door header string)

```yaml
name: Release OTA
on:
  push:
    tags: ['v*']

permissions:
  contents: write

env:
  OTA_MANUFACTURER_CODE: "0xFEFE"
  OTA_IMAGE_TYPE: "0x0002"
  MODEL_ID: "DFR-DoorSensor"

jobs:
  build-and-release:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          ref: main
          fetch-depth: 0

      - name: Derive version from tag
        id: ver
        run: |
          TAG="${GITHUB_REF_NAME#v}"
          IFS=. read -r MA MI PA <<< "$TAG"
          printf 'file_version=0x%02X%02X%02X00\n' "$MA" "$MI" "$PA" >> "$GITHUB_OUTPUT"
          echo "semver=$TAG" >> "$GITHUB_OUTPUT"
          echo "ma=$MA" >> "$GITHUB_OUTPUT"
          echo "mi=$MI" >> "$GITHUB_OUTPUT"
          echo "pa=$PA" >> "$GITHUB_OUTPUT"

      - uses: actions/setup-python@v5
        with:
          python-version: '3.x'
      - run: pip install platformio pytest

      - name: Unit tests (host tools + native)
        run: |
          python -m pytest tools/test_ota_tools.py -v
          pio test -e native

      - name: Build Zigbee firmware (version injected)
        run: |
          PLATFORMIO_BUILD_FLAGS="-DFW_VER_MAJOR=${{ steps.ver.outputs.ma }} -DFW_VER_MINOR=${{ steps.ver.outputs.mi }} -DFW_VER_PATCH=${{ steps.ver.outputs.pa }}" \
            pio run -e dfrobot_firebeetle2_esp32c6_zigbee

      - name: Wrap into .ota
        run: |
          python tools/make_ota_image.py \
            --in .pio/build/dfrobot_firebeetle2_esp32c6_zigbee/firmware.bin \
            --out firmware-${{ steps.ver.outputs.semver }}.ota \
            --manufacturer ${{ env.OTA_MANUFACTURER_CODE }} \
            --image-type ${{ env.OTA_IMAGE_TYPE }} \
            --file-version ${{ steps.ver.outputs.file_version }} \
            --header-string "DFR-DoorSensor OTA"

      - name: Update index
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

      - name: Commit index to main
        run: |
          git config user.name  "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          git add ota/index.json
          git commit -m "ota: index for ${{ github.ref_name }}" || echo "no change"
          git push origin HEAD:main
```

- [ ] **Step 2: Validate the workflow YAML locally**

Run: `cd /Users/lino/Developer/499/DFR-DoorSensor && python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/release-ota.yml')); print('yaml ok')"`
Expected: `yaml ok`. (Full run is validated in Phase C by pushing a real tag.)

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/release-ota.yml
git commit -m "ci: tag-triggered Zigbee OTA release (door identity 0xFEFE/0x0002)"
```

---

## Phase C — Deploy + end-to-end acceptance (manual; needs Pi access + token)

> Requires: SSH to the Pi (`ssh lino@499.steenkamps.org`, currently unreachable from dev — run from the LAN), a read-only fine-grained PAT, and z2m admin. Do NOT commit the token anywhere.

### Task C1: Publish repos, deploy the Pi service, wire z2m, verify a real update

**Files:** none (operational).

- [ ] **Step 1: Create the private repos + push**

```bash
# aggregator
cd /Users/lino/Developer/499/dfr-ota-server
gh repo create linosteenkamp/dfr-ota-server --private --source=. --remote=origin --push
# door-sensor CI branch -> merge via PR (or push main) so ota/ + workflow land on main
```

- [ ] **Step 2: Tag a door-sensor release and confirm CI publishes**

```bash
cd /Users/lino/Developer/499/DFR-DoorSensor
git tag v1.0.0 && git push origin v1.0.0
# Watch: gh run watch ; then confirm the Release asset + non-empty ota/index.json on main
```
Expected: a `firmware-1.0.0.ota` Release asset and `ota/index.json` with one entry (manufacturerCode 65278, imageType 2).

- [ ] **Step 3: Mint the token + deploy on the Pi**

Create a fine-grained PAT (Contents: read) scoped to `DFR-DoorSensor` (+ any other repos in `repos.conf`). On the Pi:
```bash
git clone https://github.com/linosteenkamp/dfr-ota-server /opt/dfr-ota-server
cd /opt/dfr-ota-server && sudo ./install.sh
echo "<PAT>" | sudo tee /etc/dfr-ota/token >/dev/null    # then verify chmod 600
sudoedit /etc/dfr-ota/repos.conf   # add linosteenkamp/DFR-DoorSensor
sudoedit /etc/dfr-ota/env          # BASE_URL=http://<pi-lan-ip>:8091
sudo systemctl restart dfr-ota-serve.service
sudo systemctl start dfr-ota-sync.service
journalctl -u dfr-ota-sync.service -n 30 --no-pager
```
Expected: journal shows `linosteenkamp/DFR-DoorSensor: 1 image(s) OK; served 1`.

- [ ] **Step 4: Verify the LAN index + image**

```bash
curl -s http://<pi-lan-ip>:8091/index.json
curl -sI http://<pi-lan-ip>:8091/ota/firmware-1.0.0.ota | head -1
```
Expected: index lists the door sensor with a `http://<pi-lan-ip>:8091/ota/...` url; the `.ota` returns `200 OK`.

- [ ] **Step 5: Point z2m at it + verify an end-to-end update**

Edit z2m `configuration.yaml` `ota:` (`zigbee_ota_override_index_location: http://<pi-lan-ip>:8091/index.json`, `disable_automatic_update_check: true`), restart z2m. In the z2m UI: the door sensor shows an available update; click Update; confirm it downloads, applies, reboots, and reports the new `fileVersion`. Confirm bootloader rollback is intact (device stays joined post-update).

- [ ] **Step 6: Confirm the timer keeps it fresh**

```bash
systemctl list-timers dfr-ota-sync.timer --no-pager
```
Tag a `v1.0.1`, wait one timer cycle (~15 min), confirm the served index advances to the new `fileVersion` with no manual action.

---

## Self-Review Notes

- **Spec coverage:** private Releases + RO token (A6 `GitHubFetcher`, C1) ✓; pull-based systemd timer (A6 units) ✓; combined family index via explicit `repos.conf` (A1 parser, A4/A5 merge) ✓; sha512+size verify (A3, used in A5) ✓; atomic swap + last-good retention (A5 `_publish`/`merge_indexes`, tested) ✓; LAN static serve :8091 (A6 `dfr-ota-serve`) ✓; z2m override + manual rollout (A6 README, C1) ✓; per-repo CI with door identity 0xFEFE/0x0002 (B1/B2) ✓; dedicated `dfr-ota-server` repo (Phase A, C1) ✓; Pi env facts confirmed at deploy (Phase C) ✓.
- **Placeholder scan:** `<pi-lan-ip>`/`<PAT>` are deploy-time values in the operational task (Phase C), not code placeholders. No TBD/TODO in code steps.
- **Type consistency:** `parse_repos_conf`, `rewrite_entry_url`, `verify_blob`/`VerifyError`, `merge_indexes`, `SkipRepo`, `sync_once(fetcher, repos, base_url, serve_dir, staging_root, log)`, and the fetcher contract (`get_index(repo)->list[dict]`, `download(repo, filename)->bytes`) are used identically across A1–A6.
- **Deviation from spec §4.2 step (b):** the fetcher checks `releases/latest` for existence, then reads the committed `ota/index.json` for authoritative metadata, then downloads the asset by filename — matching the spec's "reuse CI's computed metadata" intent.
