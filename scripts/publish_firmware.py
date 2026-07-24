#!/usr/bin/env python3
"""Publish a firmware build to https://waycast.io/fw/ (docs/OTA.md).

Takes the current targets/esp32p4/build/vmesh.bin, reads its embedded
version (esp_app_desc, offset 0x30 in the image), writes manifest.json,
and deploys both through the bastion to the site's static host — the
same cat-over-ssh path the site deploy uses (base64-armored: the
bastion's tty logging isn't 8-bit clean).

Usage:
  publish_firmware.py            # deploy build/vmesh.bin as stable
  publish_firmware.py --dry-run  # print what would happen
  publish_firmware.py --channel beta

v1 trust model: TLS authenticates the host (the device pins the CA
bundle). App-layer ed25519 manifest signing is the next hardening step.
"""

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
BIN = REPO / "targets/esp32p4/build/vmesh.bin"
BASTION = ["ssh", "-o", "ConnectTimeout=10", "Alviso99@80.240.18.30",
           "--", "kda-hetzner-1"]
WEBROOT = "/var/www/waycast/fw"


def app_version(binpath: pathlib.Path) -> str:
    """esp_app_desc_t sits at image offset 0x20; version[32] at +0x10."""
    with open(binpath, "rb") as f:
        f.seek(0x30)
        raw = f.read(32)
    return raw.split(b"\0", 1)[0].decode()


def run_remote(cmd: str, stdin_bytes: bytes | None = None) -> None:
    subprocess.run(BASTION + [cmd], input=stdin_bytes, check=True)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", choices=["stable", "beta"],
                    default="stable")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if not BIN.exists():
        sys.exit(f"no build at {BIN} — run idf.py build first")

    blob = BIN.read_bytes()
    ver = app_version(BIN)

    # PROJECT_VER is captured at CMake CONFIGURE time — a plain
    # `idf.py build` after new commits ships a stale version string.
    # Refuse to publish unless the image matches the tree.
    git_ver = subprocess.run(
        ["git", "describe", "--tags", "--always", "--dirty"],
        cwd=REPO, capture_output=True, text=True).stdout.strip()
    if ver != git_ver[:31]:
        sys.exit(f"STALE BUILD: image says {ver!r} but the tree is "
                 f"{git_ver!r} — run `idf.py reconfigure build` first")
    if ver.endswith("-dirty"):
        sys.exit("refusing to publish a -dirty build — commit first")
    sha = hashlib.sha256(blob).hexdigest()
    binname = f"vmesh-{ver}.bin"
    manifest_name = ("manifest.json" if args.channel == "stable"
                     else "manifest-beta.json")
    manifest = json.dumps({
        "version": ver,
        "url": f"https://waycast.io/fw/{binname}",
        "size": len(blob),
        "sha256": sha,
        "hw": "esp32p4-devkit",
    }, indent=2) + "\n"

    print(f"version {ver}  ({len(blob)} bytes, sha256 {sha[:16]}...)")
    print(f"-> {WEBROOT}/{binname}")
    print(f"-> {WEBROOT}/{manifest_name}")
    if args.dry_run:
        print(manifest)
        return

    import base64
    b64 = base64.b64encode(blob)
    run_remote(f"mkdir -p {WEBROOT}")
    run_remote(f"base64 -d > {WEBROOT}/{binname}", b64)
    # verify the armored transfer before the manifest goes live
    out = subprocess.run(
        BASTION + [f"sha256sum {WEBROOT}/{binname}"],
        capture_output=True, text=True, check=True).stdout
    if sha not in out:
        sys.exit(f"UPLOAD CORRUPT: remote hash mismatch\n{out}")
    run_remote(f"cat > {WEBROOT}/{manifest_name}",
               manifest.encode())
    print("published + hash-verified.")


if __name__ == "__main__":
    main()
