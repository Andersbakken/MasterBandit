#!/usr/bin/env python3
"""Decode an mb crash log against a sidecar debug bundle.

Usage:
    decode-crash [--sidecar PATH] [--repo OWNER/REPO] [--release TAG]
                 [--cache-dir DIR] [--no-download] CRASH_LOG

If --sidecar is given, the path may be a .debug / .debug.gz / .dSYM /
.dSYM.tar.gz; the script handles unpacking. Without --sidecar the
matching artifact is fetched from the named GitHub release (default
'nightly') and cached. Set $GITHUB_TOKEN to avoid the 60 req/hr
unauthenticated rate limit.
"""

import argparse
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

DEFAULT_REPO = os.environ.get("MB_REPO", "Andersbakken/MasterBandit")
DEFAULT_RELEASE = "nightly"
DEFAULT_CACHE = Path.home() / ".cache" / "mb-decode"


def parse_crash_log(path):
    text = Path(path).read_text(errors="replace")
    info = {"build_id": None, "version": None, "signal": None,
            "platform": None, "main_base": None, "main_path": None,
            "frames": []}

    if m := re.search(r"^build-id:\s*(\S+)", text, re.M):
        info["build_id"] = m.group(1)
    if m := re.search(r"^version:\s*(.+)$", text, re.M):
        info["version"] = m.group(1).strip()
    if m := re.search(r"^signal:\s*(\d+)", text, re.M):
        info["signal"] = int(m.group(1))

    if "images:" in text:
        info["platform"] = "macos"
        for m in re.finditer(r"^\s*base=(\S+)\s+slide=\S+\s+(.+)$", text, re.M):
            name = m.group(2).strip()
            if name.endswith("/mb") or "/MacOS/mb" in name:
                info["main_base"] = int(m.group(1), 16)
                info["main_path"] = name
                break
    elif re.search(r"^[0-9a-f]+-[0-9a-f]+\s+r", text, re.M):
        info["platform"] = "linux"
        for m in re.finditer(
            r"^([0-9a-f]+)-[0-9a-f]+\s+r-xp\s+0+\s+\S+\s+\S+\s+(/\S+)$",
            text, re.M,
        ):
            path = m.group(2).strip()
            if ".so" not in path and "/ld-" not in path:
                info["main_base"] = int(m.group(1), 16)
                info["main_path"] = path
                break

    in_backtrace = False
    for line in text.splitlines():
        if line.startswith("backtrace:"):
            in_backtrace = True
            continue
        if not in_backtrace:
            continue
        if addrs := re.findall(r"0x[0-9a-fA-F]+", line):
            info["frames"].append(int(addrs[-1], 16))

    return info


def gh_api_get(url):
    headers = {"Accept": "application/vnd.github+json"}
    if tok := os.environ.get("GITHUB_TOKEN"):
        headers["Authorization"] = f"Bearer {tok}"
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req) as r:
        return json.load(r)


def fetch_release_asset(repo, release, asset_name, cache_dir):
    cache_dir.mkdir(parents=True, exist_ok=True)
    dest = cache_dir / f"{release}__{asset_name}"
    if dest.exists():
        return dest
    data = gh_api_get(f"https://api.github.com/repos/{repo}/releases/tags/{release}")
    for asset in data.get("assets", []):
        if asset["name"] == asset_name:
            print(f"downloading {asset_name} from {repo}@{release}...", file=sys.stderr)
            with urllib.request.urlopen(asset["browser_download_url"]) as r:
                dest.write_bytes(r.read())
            return dest
    raise FileNotFoundError(f"{asset_name} not in release {release} of {repo}")


def expand_sidecar(sidecar, platform, cache_dir):
    sidecar = Path(sidecar)
    if platform == "linux":
        if sidecar.suffix == ".gz":
            out = cache_dir / sidecar.name[:-3]
            if not out.exists():
                with gzip.open(sidecar) as src, open(out, "wb") as dst:
                    shutil.copyfileobj(src, dst)
            return out
        return sidecar
    if platform == "macos":
        if sidecar.name.endswith(".tar.gz") or sidecar.suffix == ".tgz":
            extract_dir = cache_dir / f"_extract_{sidecar.stem}"
            extract_dir.mkdir(parents=True, exist_ok=True)
            with tarfile.open(sidecar) as tf:
                tf.extractall(extract_dir)
            for child in extract_dir.iterdir():
                if child.suffix == ".dSYM" or child.name.endswith(".dSYM"):
                    return child
            raise RuntimeError(f"no .dSYM bundle found inside {sidecar}")
        return sidecar
    raise RuntimeError(f"unsupported platform: {platform}")


def sidecar_build_id(sidecar, platform):
    if platform == "linux":
        out = subprocess.run(["readelf", "-n", str(sidecar)],
                             capture_output=True, text=True)
        if m := re.search(r"Build ID:\s+([0-9a-f]+)", out.stdout):
            return m.group(1).lower()
    elif platform == "macos":
        dwarf_dir = Path(sidecar) / "Contents" / "Resources" / "DWARF"
        if dwarf_dir.is_dir():
            for f in dwarf_dir.iterdir():
                out = subprocess.run(["dwarfdump", "--uuid", str(f)],
                                     capture_output=True, text=True)
                if m := re.search(r"UUID:\s+([A-F0-9-]+)", out.stdout):
                    return m.group(1).replace("-", "").lower()
    return None


def symbolicate(info, sidecar):
    if not info["frames"]:
        return "(no frame addresses found in crash log)\n"
    if info["platform"] == "linux":
        offsets = [f"0x{(a - info['main_base']):x}" for a in info["frames"]]
        cmd = ["addr2line", "-i", "-f", "-C", "-e", str(sidecar)] + offsets
    else:
        dwarf_dir = Path(sidecar) / "Contents" / "Resources" / "DWARF"
        dwarf_file = next(dwarf_dir.iterdir())
        offsets = [f"0x{a:x}" for a in info["frames"]]
        cmd = ["atos", "-o", str(dwarf_file),
               "-l", f"0x{info['main_base']:x}"] + offsets
    out = subprocess.run(cmd, capture_output=True, text=True)
    return out.stdout or out.stderr


def main():
    ap = argparse.ArgumentParser(description="Decode an mb crash log.")
    ap.add_argument("log")
    ap.add_argument("--sidecar")
    ap.add_argument("--repo", default=DEFAULT_REPO)
    ap.add_argument("--release", default=DEFAULT_RELEASE)
    ap.add_argument("--cache-dir", default=str(DEFAULT_CACHE))
    ap.add_argument("--no-download", action="store_true")
    args = ap.parse_args()

    info = parse_crash_log(args.log)
    for required in ("build_id", "platform", "main_base"):
        if info[required] is None:
            print(f"error: crash log missing {required}", file=sys.stderr)
            return 2

    cache_dir = Path(args.cache_dir)
    cache_dir.mkdir(parents=True, exist_ok=True)

    if args.sidecar:
        sidecar_raw = Path(args.sidecar)
    elif args.no_download:
        print("error: --no-download set and no --sidecar provided", file=sys.stderr)
        return 2
    else:
        asset = ("mb-linux-x64.debug.gz" if info["platform"] == "linux"
                 else "mb-macos-arm64.dSYM.tar.gz")
        sidecar_raw = fetch_release_asset(args.repo, args.release, asset, cache_dir)

    sidecar = expand_sidecar(sidecar_raw, info["platform"], cache_dir)
    found_bid = sidecar_build_id(sidecar, info["platform"])
    if found_bid and found_bid != info["build_id"].lower():
        print(f"warning: sidecar build-id {found_bid} != crash build-id "
              f"{info['build_id']}; symbolication will be wrong. "
              f"Try --release <other-tag> or --sidecar PATH.",
              file=sys.stderr)

    print(f"=== mb crash (decoded) ===")
    print(f"version:  {info['version']}")
    print(f"build-id: {info['build_id']}")
    print(f"signal:   {info['signal']}")
    print(f"platform: {info['platform']}")
    print(f"frames:   {len(info['frames'])}")
    print()
    print(symbolicate(info, sidecar))
    return 0


if __name__ == "__main__":
    sys.exit(main())
