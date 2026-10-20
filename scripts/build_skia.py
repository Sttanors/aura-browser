"""Build the M0 Skia configuration using only locked source dependencies."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
from dependencies import ROOT, host


def build(jobs: int, diagnostic_angle: bool = False) -> None:
    platform = host()
    source = ROOT / "third_party/src/skia"
    tools = ROOT / "third_party/tools" / platform
    suffix = ".exe" if os.name == "nt" else ""
    output = ROOT / "third_party/skia-build" / platform
    args = {
        "is_official_build": True, "is_debug": False, "is_component_build": False,
        "is_trivial_abi": False, "target_cpu": "x64",
        "skia_enable_tools": False, "skia_enable_skottie": False,
        "skia_enable_svg": False, "skia_enable_pdf": False,
        "skia_enable_ganesh": True, "skia_enable_graphite": False,
        "skia_use_gl": os.name != "nt" or diagnostic_angle, "skia_use_vulkan": False,
        "skia_use_angle": os.name == "nt" and diagnostic_angle, "skia_use_partition_alloc": False,
        "skia_use_harfbuzz": False, "skia_use_icu": False,
        "skia_use_fontconfig": False, "skia_use_freetype": os.name != "nt",
        "skia_use_system_freetype2": False, "skia_use_freetype_zlib_bundled": False,
        "skia_use_system_libpng": False, "skia_use_system_zlib": False,
        "skia_use_expat": False, "skia_use_piex": False,
        "skia_use_libjpeg_turbo_decode": False, "skia_use_libjpeg_turbo_encode": False,
        "skia_use_libwebp_decode": False, "skia_use_libwebp_encode": False,
        "skia_use_libpng_decode": False, "skia_use_libpng_encode": True,
        "skia_use_wuffs": False, "skia_use_perfetto": False,
    }
    if os.name == "nt":
        compiler = shutil.which("clang-cl")
        if not compiler:
            raise RuntimeError("Install LLVM and run from x64 Native Tools Command Prompt")
        args["clang_win"] = str(Path(compiler).resolve().parents[1]).replace("\\", "/")
        args["extra_cflags"] = ["/MT", "/utf-8"]
        args.update(skia_gl_standard="gles", angle_enable_d3d11=True,
                    angle_enable_gl=False, angle_enable_vulkan=False, angle_enable_null=False,
                    angle_build_tests=False, angle_enable_abseil=False)
    else:
        args.update(cc="clang", cxx="clang++", skia_use_egl=True, skia_gl_standard="gles")
    text = "\n".join(f"{key} = {json.dumps(value)}" for key, value in args.items()) + "\n"
    output.mkdir(parents=True, exist_ok=True)
    (output / "args.gn").write_text(text, encoding="utf-8")
    subprocess.run([str(tools / f"gn{suffix}"), "gen", str(output)], cwd=source, check=True)
    targets = ["skia", "aura_angle"] if os.name == "nt" and diagnostic_angle else ["skia"]
    subprocess.run([str(tools / f"ninja{suffix}"), "-C", str(output), f"-j{jobs}", *targets], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    build(max(1, parser.parse_args().jobs))
