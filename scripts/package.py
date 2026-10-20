"""Create either a complete source archive or a portable browser runtime archive."""
from __future__ import annotations
import argparse
from pathlib import Path
import shutil
import tempfile
import zipfile
from dependencies import ROOT, host


def source_archive(output: Path):
    directories = ("app", "common", "core", "render", "ui", "platform", "cmake", "design", "scripts", "tests", "docs", ".github")
    files = [ROOT / name for name in ("CMakeLists.txt", "CMakePresets.json", "README.md", ".gitignore", ".clang-format", "run.sh", "run.ps1", "vcpkg.json", ".gitattributes")]
    for directory in directories:
        files += [p for p in (ROOT / directory).rglob("*") if p.is_file() and "__pycache__" not in p.parts]
    files += [ROOT / "resources/default.toml", ROOT / "third_party/dependencies.lock.json"]
    files += list((ROOT / "third_party/patches").glob("*"))
    files += [p for folder in ("vendor", "protection") for p in (ROOT/"resources"/folder).rglob("*") if p.is_file()]
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, strict_timestamps=False) as archive:
        for file in sorted(set(files)):
            if file.is_file(): archive.write(file, Path("Aura2") / file.relative_to(ROOT))


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
                if source.is_file() and source.suffix not in {".lib", ".a", ".pdb"}:
                    target = stage / source.relative_to(cef / folder)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, target)
        shutil.copy2(executable, stage / executable.name)
        if platform == "windows64":
            for name in ("aura_browser.dll", "libEGL.dll", "libGLESv2.dll"):
                shutil.copy2(build / name, stage / name)
        shutil.copytree(ROOT / "resources/fonts", stage / "fonts")
        for folder in ("vendor", "protection"):
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
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, strict_timestamps=False) as archive:
            for source in sorted(stage.rglob("*")):
                if source.is_file(): archive.write(source, Path("Aura2") / source.relative_to(stage))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output or ROOT / "artifacts" / ("aura2-browser-source.zip" if args.source else f"aura2-{host()}.zip")
    output.parent.mkdir(parents=True, exist_ok=True)
    (source_archive if args.source else runtime_archive)(output)
    print(output)
