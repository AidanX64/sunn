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

OS_ARCH_TO_TRIPLET = {
    ("windows", "x86_64"): "x64-windows",
    ("windows", "aarch64"): "arm64-windows",
    ("linux", "x86_64"): "x64-linux",
    ("linux", "aarch64"): "arm64-linux",
    ("macos", "x86_64"): "x64-macos",
    ("macos", "aarch64"): "arm64-macos",
}


def parse_toml_list(text: str, key: str) -> list[str]:
    m = re.search(rf"{key}\s*=\s*\[([^\]]*)\]", text)
    if not m:
        return []
    return re.findall(r'"([^"]+)"', m.group(1))


def parse_scalar(text: str, key: str) -> str:
    m = re.search(rf'{key}\s*=\s*"([^"]+)"', text)
    return m.group(1) if m else ""


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
        oss = parse_toml_list(manifest, r"\bos\b")
        arches = parse_toml_list(manifest, r"\barch\b")
        triplets = sorted(
            {OS_ARCH_TO_TRIPLET.get((o, a), f"{a}-{o}") for o in oss for a in arches}
        )

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
            "description": f"Minimal {lang} library fixture for the sunn native registry (ships objects, never a main).",
            "license": "MIT",
            "homepage": "https://example.com/" + name,
            "lang": lang,
            "build": "forge",
            "triplets": triplets,
            "dependencies": [],
            "artifacts": [
                {"triplet": t, "url": url, "sha256": sha256} for t in triplets
            ],
            "forge": {"manifest": f"/packages/{name}/Forge.toml"},
        }
        (pkg_dir / f"{version}.json").write_text(json.dumps(pkg_json, indent=2) + "\n")
        (pkg_dir / "Forge.toml").write_text(manifest)
        entries[name] = {
            "name": name,
            "latest": version,
            "description": pkg_json["description"],
            "license": "MIT",
            "homepage": pkg_json["homepage"],
            "index": f"/packages/{name}/{version}.json",
        }
        print(f"{name} {version}: {tarball.name} sha256={sha256[:16]}... ({len(triplets)} triplets)")

    index["packages"] = [entries[k] for k in sorted(entries)]
    index_path.write_text(json.dumps(index, indent=2) + "\n")
    print(f"index: {index_path} ({len(entries)} packages)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
