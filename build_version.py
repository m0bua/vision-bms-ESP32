Import("env")

from datetime import datetime, timezone
from pathlib import Path
from shutil import copy2
import json
import subprocess


PROJECT_DIR = Path(env["PROJECT_DIR"])
GENERATED_DIR = PROJECT_DIR / ".pio" / "build" / "generated"
GENERATED_DIR.mkdir(parents=True, exist_ok=True)
env.Append(CPPPATH=[str(GENERATED_DIR)])


def _run_git(*args):
    return subprocess.check_output(
        ["git", "-C", str(PROJECT_DIR), *args],
        stderr=subprocess.DEVNULL,
        text=True,
    ).strip()


def _format_stamp(epoch_ns):
    dt = datetime.fromtimestamp(epoch_ns / 1_000_000_000, tz=timezone.utc)
    return dt.strftime("%Y-%m-%d %H:%M:%S.") + f"{(epoch_ns // 1_000_000) % 1000:03d}Z"


def _latest_source_mtime_ns():
    candidates = [PROJECT_DIR / "platformio.ini"]
    for folder_name in ("src", "lib"):
        folder = PROJECT_DIR / folder_name
        if not folder.exists():
            continue
        for path in folder.rglob("*"):
            if path.is_file():
                candidates.append(path)
    if not candidates:
        return int(datetime.now(tz=timezone.utc).timestamp() * 1_000_000_000)
    latest = max(candidates, key=lambda path: path.stat().st_mtime_ns)
    return latest.stat().st_mtime_ns


def _build_metadata():
    try:
        dirty = subprocess.run(
            ["git", "-C", str(PROJECT_DIR), "status", "--porcelain"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout.strip() != ""
        if dirty:
            stamp_ns = _latest_source_mtime_ns()
            dt = datetime.fromtimestamp(stamp_ns / 1_000_000_000, tz=timezone.utc)
            millis = (stamp_ns // 1_000_000) % 1000
            version = dt.strftime("%Y%m%d%H%M-%S") + f"{millis:03d}"
            stamp = _format_stamp(stamp_ns)
            source = "files"
            title = f"{source} {stamp}"
        else:
            commit_hash = _run_git("rev-parse", "HEAD")
            commit_short = commit_hash[:8]
            commit_date = _run_git("show", "-s", "--format=%cI", "HEAD")
            dt = datetime.fromisoformat(commit_date.replace("Z", "+00:00")).astimezone(timezone.utc)
            version = dt.strftime("%Y%m%d%H%M")
            stamp = dt.strftime("%Y-%m-%d %H:%MZ")
            source = f"git {commit_short}"
            title = f"{source} {stamp}"
        return {
            "version": version,
            "source": source,
            "stamp": stamp,
            "title": title,
        }
    except Exception:
        stamp_ns = int(datetime.now(tz=timezone.utc).timestamp() * 1_000_000_000)
        dt = datetime.fromtimestamp(stamp_ns / 1_000_000_000, tz=timezone.utc)
        millis = (stamp_ns // 1_000_000) % 1000
        version = dt.strftime("%Y%m%d%H%M-%S") + f"{millis:03d}"
        stamp = _format_stamp(stamp_ns)
        return {
            "version": version,
            "source": "local",
            "stamp": stamp,
            "title": f"local {stamp}",
        }


meta = _build_metadata()


def _copy_versioned_bin(source, target, env):
    candidates = [Path(str(node)) for node in list(source) + list(target)]
    bin_path = next((path for path in candidates if path.suffix.lower() == ".bin" and path.name == "firmware.bin"), None)
    if bin_path is None or not bin_path.exists():
        return
    versioned_bin = bin_path.with_name(f"firmware-{meta['version']}.bin")
    copy2(bin_path, versioned_bin)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _copy_versioned_bin)

header = GENERATED_DIR / "build_info_generated.h"
header.write_text(
    "\n".join(
        [
            "#pragma once",
            f"#define BUILD_VERSION_STRING {json.dumps(meta['version'])}",
            f"#define BUILD_VERSION_SOURCE {json.dumps(meta['source'])}",
            f"#define BUILD_VERSION_STAMP {json.dumps(meta['stamp'])}",
            f"#define BUILD_VERSION_TITLE {json.dumps(meta['title'])}",
            "",
        ]
    ),
    encoding="utf-8",
)
