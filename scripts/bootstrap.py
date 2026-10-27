"""Fetch verified dependencies and build Skia. No package-manager side effects."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import sys
from dependencies import ROOT, download, host, install, install_file


def main() -> None:
    if sys.version_info < (3, 12):
        raise RuntimeError("Python 3.12+ is required")
    parser = argparse.ArgumentParser()
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--diagnostic-angle", action="store_true", help="Build the optional Windows M0 GPU harness")
    parser.add_argument("--fetch-only", action="store_true")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    platform = host()
    install(f"cef-{platform}", ROOT / "third_party/src" / f"cef-{platform}", args.offline)
    if os.name != "nt":
        install("sdl3", ROOT / "third_party/src/sdl3", args.offline)
        from patch_sdl_wayland import patch as patch_wayland
        patch_wayland()
    skia = install("skia", ROOT / "third_party/src/skia", args.offline)
    for name in ["googletest", "spdlog", "tomlplusplus"]:
        install(name, ROOT / "third_party/src" / name, args.offline)
    for name in ["freetype", "libpng", "zlib"]:
        install(name, skia / "third_party/externals" / name, args.offline)
    if os.name == "nt" and args.diagnostic_angle:
        install("angle", skia / "third_party/externals/angle2", args.offline)
        install("zlib-google-utils", skia / "third_party/externals/zlib/google", args.offline)
        for name in ("spirv-tools", "spirv-headers"):
            install(name, skia / "third_party/externals" / name, args.offline)
        patch_angle_response_file(skia)
    patch = ROOT / "third_party/patches/skia-zlib.BUILD.gn"
    shutil.copyfile(patch, skia / "third_party/zlib/BUILD.gn")
    build_file = skia / "BUILD.gn"
    original = build_file.read_text(encoding="utf-8")
    if 'group("aura_angle")' not in original:
        build_file.write_text(original + (ROOT / "third_party/patches/skia-angle.gn").read_text(), encoding="utf-8")
    for name in ["gn", "ninja"]:
        temporary = install(f"{name}-{platform}", ROOT / "third_party/tools" / f"{name}-{platform}", args.offline)
        binary = name + (".exe" if os.name == "nt" else "")
        target = ROOT / "third_party/tools" / platform / binary
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(temporary / binary, target)
        target.chmod(0o755)
    install_file("inter", ROOT / "resources/fonts/InterVariable.ttf", args.offline)
    install_file("inter-license", ROOT / "resources/fonts/OFL.txt", args.offline)
    if not args.fetch_only:
        from build_skia import build
        build(max(1, args.jobs), args.diagnostic_angle)
    print(f"Aura M0 dependencies ready: {platform}")


def patch_angle_response_file(skia: Path) -> None:
    script = skia / "third_party/externals/angle2/src/program_serialize_data_version.py"
    original = script.read_text(encoding="utf-8")
    before = 'with open(response_file_name, "r") as input_files_for_hash_generation:\n    angle_code_files = shlex.split(input_files_for_hash_generation)'
    after = ('# Aura: GN shell-escapes non-ASCII path bytes in response files.\n'
             'with open(response_file_name, "rb") as input_files_for_hash_generation:\n'
             '    import os\n'
             '    raw = input_files_for_hash_generation.read().decode("latin-1")\n'
             '    angle_code_files = [os.fsdecode(s.encode("latin-1")) for s in shlex.split(raw)]')
    if before in original:
        script.write_text(original.replace(before, after), encoding="utf-8")
    elif after not in original:
        raise RuntimeError("ANGLE response-file patch does not match the locked source")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f"Bootstrap failed: {error}", file=sys.stderr)
        sys.exit(1)
