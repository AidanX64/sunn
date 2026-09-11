#!/usr/bin/env python3
"""Build sunn native-registry fixtures.

For each directory in registry-fixtures/ (a Forge.toml + src/ fixture):
  - tars it to public/packages/<name>/<name>-<version>.tar.gz
  - computes sha256
  - writes public/packages/<name>/<version>.json (sunn native schema)
  - upserts public/packages/sunn.registry.json

One SOURCE tarball per version; every supported triplet references the
same file+hash. Per-triplet *binary* artifacts are a later CI phase
(see public/packages/README.md).

Usage: python3 scripts/package_registry_fixtures.py  (run from repo root)
"""
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "registry-fixtures"
OUT = ROOT / "public" / "packages"

def parse_toml_list(text: str, key: str) -> list[str]:
    m = re.search(rf"{key}\s*=\s*\[([^\]]*)\]", text)
    if not m:
        return []
    return re.findall(r'"([^"]+)"', m.group(1))


def parse_scalar(text: str, key: str) -> str:
    m = re.search(rf'{key}\s*=\s*"([^"]+)"', text)
    return m.group(1) if m else ""


def fixture_revision(fixture: Path) -> int:
    """Recipe revision for a fixture (vcpkg port-version analog).

    Read from an optional `<fixture>/.revision` sidecar so a recipe fix
    (new tarball bytes for the same upstream version) becomes an
    addressable, updatable pin without a version bump. Defaults to 0.
    """
    try:
        raw = (fixture / ".revision").read_text().strip()
    except FileNotFoundError:
        return 0
    if not raw.isdigit():
        print(f"warn {fixture.name}: bad .revision {raw!r}, using 0", file=sys.stderr)
        return 0
    return min(int(raw), 1000000)


def semver_sort_key(version: str) -> tuple:
    """Descending-sort key: numeric MAJOR/MINOR/PATCH, release over prerelease."""
    core, _, pre = version.partition("-")
    try:
        nums = tuple(int(p) for p in core.split("."))
    except ValueError:
        nums = (0,)
    while len(nums) < 3:
        nums = nums + (0,)
    return (nums, 1 if not pre else 0, pre)


def collect_versions(pkg_dir: Path) -> list[dict]:
    """Merge every <version>.json on disk into a versions[] list, newest first."""
    found = []
    for cand in sorted(pkg_dir.glob("*.json")):
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?", cand.stem):
            continue
        try:
            data = json.loads(cand.read_text())
        except (json.JSONDecodeError, OSError):
            continue
        if data.get("version") != cand.stem:
            continue
        rev = data.get("revision", 0)
        found.append({"version": cand.stem,
                      "revision": rev if isinstance(rev, int) and rev >= 0 else 0})
    found.sort(key=lambda e: semver_sort_key(e["version"]), reverse=True)
    return found


def main() -> int:
    if not FIXTURES.is_dir():
        print(f"no fixtures dir: {FIXTURES}", file=sys.stderr)
        return 1
    index_path = OUT / "sunn.registry.json"
    try:
        index = json.loads(index_path.read_text())
    except FileNotFoundError:
        index = {"$schema": "", "name": "sunn-native", "description": "", "packages": []}
    entries: dict[str, dict] = {p["name"]: p for p in index.get("packages", [])}

    for fixture in sorted(p for p in FIXTURES.iterdir() if p.is_dir()):
        manifest = (fixture / "Forge.toml").read_text()
        name = parse_scalar(manifest, "name")
        version = parse_scalar(manifest, "version")
        if not name or not version:
            print(f"skip {fixture.name}: missing [project] name/version", file=sys.stderr)
            continue
        c = parse_toml_list(manifest, r"\bc\b")
        cpp = parse_toml_list(manifest, r"\bcpp\b")
        asm = parse_toml_list(manifest, r"\basm\b")
        lang = "c" if c else ("c++" if cpp else "asm")
        rev = fixture_revision(fixture)
        pkg_dir = OUT / name
        pkg_dir.mkdir(parents=True, exist_ok=True)
        tarball = pkg_dir / f"{name}-{version}.tar.gz"
        members = ["Forge.toml", "src"] + (
            ["include"] if (fixture / "include").is_dir() else []
        )
        subprocess.run(
            ["tar", "-czf", str(tarball), "-C", str(fixture), *members],
            check=True,
        )
        sha256 = hashlib.sha256(tarball.read_bytes()).hexdigest()
        url = f"/packages/{name}/{tarball.name}"

        pkg_json = {
            "name": name,
            "version": version,
            "revision": rev,
            "description": f"Minimal {lang} library fixture for the sunn native registry (ships objects, never a main).",
            "license": "MIT",
            "homepage": "https://example.com/" + name,
            "lang": lang,
            "build": "forge",
            "dependencies": [],
            "source": {"kind": "url", "location": url, "sha256": sha256},
            "patches": [],
            "forge": {"manifest": f"/packages/{name}/Forge.toml"},
        }
        (pkg_dir / f"{version}.json").write_text(json.dumps(pkg_json, indent=2) + "\n")
        (pkg_dir / "Forge.toml").write_text(manifest)
        versions = collect_versions(pkg_dir)
        newest = versions[0] if versions else {"version": version, "revision": rev}
        entries[name] = {
            "name": name,
            "latest": newest["version"],
            "latest_revision": newest["revision"],
            "description": pkg_json["description"],
            "license": "MIT",
            "homepage": pkg_json["homepage"],
            "index": f"/packages/{name}/{newest['version']}.json",
            "versions": versions,
        }
        print(f"{name} {version} rev {rev}: {tarball.name} sha256={sha256[:16]}...")

    index["packages"] = [entries[k] for k in sorted(entries)]
    index_path.write_text(json.dumps(index, indent=2) + "\n")
    print(f"index: {index_path} ({len(entries)} packages)")
    baseline = {
        "name": "sunn-native-baseline",
        "baseline": [
            {"name": k, "version": entries[k]["latest"],
             "revision": entries[k]["latest_revision"]}
            for k in sorted(entries)
        ],
    }
    baseline_path = OUT.parent / "baseline.json"
    baseline_path.write_text(json.dumps(baseline, indent=2) + "\n")
    print(f"baseline: {baseline_path} ({len(entries)} pins)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
