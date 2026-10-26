"""Create a portable browser runtime archive; GitHub supplies source snapshots."""
from __future__ import annotations
import argparse
from pathlib import Path
import shutil
import tempfile
import zipfile
from dependencies import ROOT, host


def runtime_archive(output: Path):
    platform = host()
    build = ROOT / "build" / ("windows-x64-release" if platform == "windows64" else "linux-x64-release")
    executable = build / ("aura_browser.exe" if platform == "windows64" else "aura_browser")
    if not executable.is_file(): raise RuntimeError("Build Aura before packaging")
    with tempfile.TemporaryDirectory(prefix="aura-package-") as temporary:
        stage = Path(temporary)
        cef = ROOT / "third_party/src" / f"cef-{platform}"
        for folder in ("Release", "Resources"):
            for source in (cef / folder).rglob("*"):
                if source.is_file() and source.name != "bootstrap.exe" and source.suffix not in {".lib", ".a", ".pdb"}:
                    target = stage / source.relative_to(cef / folder)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, target)
        shutil.copy2(executable, stage / executable.name)
        if platform == "windows64":
            for name in ("aura_browser.dll",):
                shutil.copy2(build / name, stage / name)
        shutil.copytree(ROOT / "resources/fonts", stage / "fonts")
        for folder in ("vendor", "protection", "branding", "reader", "Dictionaries"):
            shutil.copytree(ROOT/"resources"/folder, stage/folder)
        shutil.copy2(ROOT / "resources/default.toml", stage / "default.toml")
        license_names = ("LICENSE", "LICENSE.txt", "LICENSE.TXT", "OFL.txt", "CREDITS.html", "FTL.TXT", "zlib.h")
        for name, source in [("cef", cef), ("skia", ROOT / "third_party/src/skia"),
                             ("spdlog", ROOT / "third_party/src/spdlog"),
                             ("tomlplusplus", ROOT / "third_party/src/tomlplusplus"),
                             ("inter", ROOT / "resources/fonts"),
                             ("freetype", ROOT / "third_party/src/skia/third_party/externals/freetype/docs"),
                             ("libpng", ROOT / "third_party/src/skia/third_party/externals/libpng"),
                             ("zlib", ROOT / "third_party/src/skia/third_party/externals/zlib"),
                             ("angle", ROOT / "third_party/src/skia/third_party/externals/angle2")]:
            for filename in license_names:
                candidate = source / filename
                if candidate.is_file():
                    target = stage / "licenses" / name / filename
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(candidate, target)
        if platform == "windows64":
            share = build / "vcpkg_installed/x64-windows-static/share"
            if not share.is_dir():
                share = ROOT / "vcpkg_installed/x64-windows-static/share"
            notices = list(share.glob("*/copyright"))
            if not notices:
                raise RuntimeError("Windows static dependency license notices are missing")
            for notice in notices:
                target = stage / "licenses" / notice.parent.name / "copyright"
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(notice, target)
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, strict_timestamps=False) as archive:
            for source in sorted(stage.rglob("*")):
                if source.is_file(): archive.write(source, Path("Aura2") / source.relative_to(stage))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output or ROOT / "artifacts" / f"aura2-{host()}.zip"
    output.parent.mkdir(parents=True, exist_ok=True)
    runtime_archive(output)
    print(output)
