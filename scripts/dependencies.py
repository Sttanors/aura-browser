"""Locked downloads and safe, atomic extraction. Python 3.12+; standard library only."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LOCK_PATH = ROOT / "third_party/dependencies.lock.json"


def lock() -> dict:
    return json.loads(LOCK_PATH.read_text(encoding="utf-8"))["artifacts"]


def checksum(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download(name: str, offline: bool = False) -> Path:
    entry = lock()[name]
    target = ROOT / "third_party/cache" / entry["archive"]
    if target.is_file() and checksum(target) == entry["sha256"]:
        return target
    if offline:
        raise RuntimeError(f"Missing or corrupt cached dependency: {name}")
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_suffix(target.suffix + ".partial")
    print(f"Downloading {name} {entry['version']}", flush=True)
    try:
        request = urllib.request.Request(entry["url"], headers={"User-Agent": "Aura2-bootstrap/0.1"})
        with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as stream:
            shutil.copyfileobj(response, stream, 1024 * 1024)
        if checksum(partial) != entry["sha256"]:
            raise RuntimeError(f"SHA-256 mismatch: {name}")
        partial.replace(target)
    finally:
        partial.unlink(missing_ok=True)
    return target


def extract_archive(archive: Path, destination: Path, strip_root: bool) -> None:
    """Reject traversal, devices, and links before changing the destination."""
    def relative(name: str) -> str | None:
        name = name.replace("\\", "/")
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts or any(":" in p for p in path.parts):
            raise ValueError(f"Unsafe archive path: {name}")
        parts = path.parts[1:] if strip_root else path.parts
        return "/".join(parts) if parts else None

    destination.mkdir(parents=True, exist_ok=True)
    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as source:
            entries = [(entry, relative(entry.filename)) for entry in source.infolist()]
            if sum(entry.file_size for entry, _ in entries) > 8 * 1024**3:
                raise ValueError("Archive exceeds extraction budget")
            for entry, name in entries:
                if ((entry.external_attr >> 16) & 0o170000) == 0o120000:
                    raise ValueError("Archive symlinks are not accepted")
                if name:
                    target = destination / name
                    if entry.is_dir():
                        target.mkdir(parents=True, exist_ok=True)
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with source.open(entry) as src, target.open("wb") as dst:
                            shutil.copyfileobj(src, dst)
    else:
        with tarfile.open(archive) as source:
            entries = [(entry, relative(entry.name)) for entry in source.getmembers()]
            if sum(entry.size for entry, _ in entries) > 8 * 1024**3:
                raise ValueError("Archive exceeds extraction budget")
            for entry, name in entries:
                if not (entry.isfile() or entry.isdir()):
                    raise ValueError(f"Archive links/devices are not accepted: {entry.name}")
                if name:
                    target = destination / name
                    if entry.isdir():
                        target.mkdir(parents=True, exist_ok=True)
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with source.extractfile(entry) as src, target.open("wb") as dst:
                            shutil.copyfileobj(src, dst)
                        target.chmod(entry.mode & 0o777)


def install(name: str, destination: Path, offline: bool = False) -> Path:
    entry = lock()[name]
    archive = download(name, offline)
    marker = destination / ".aura-artifact.json"
    if marker.is_file() and json.loads(marker.read_text())["sha256"] == entry["sha256"]:
        return destination
    if destination.exists():
        raise RuntimeError(f"Unmanaged or different dependency directory: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".aura-extract-", dir=destination.parent))
    try:
        extract_archive(archive, stage, entry.get("strip_root", True))
        (stage / ".aura-artifact.json").write_text(json.dumps(entry, indent=2) + "\n")
        stage.replace(destination)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    return destination


def install_file(name: str, destination: Path, offline: bool = False) -> None:
    source = download(name, offline)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or checksum(destination) != lock()[name]["sha256"]:
        shutil.copyfile(source, destination)


def host() -> str:
    import platform
    if platform.machine().lower() not in {"amd64", "x86_64"}:
        raise RuntimeError("M0 supports x64 hosts only")
    if os.name == "nt":
        return "windows64"
    if platform.system() == "Linux":
        return "linux64"
    raise RuntimeError("M0 supports Windows and Linux hosts")
