"""Package CEF's sandbox bootstrap and privately built ANGLE without modifying the SDK."""
import shutil
import subprocess
import sys
from pathlib import Path

destination, cef, skia = map(Path, sys.argv[1:4])
executable = destination / ((sys.argv[5] if len(sys.argv) > 5 else "aura_diagnostic") + ".exe")
shutil.copyfile(cef / "Release/bootstrap.exe", executable)
for filename in ("libEGL.dll", "libGLESv2.dll"):
    source = skia / filename
    if not source.is_file():
        raise RuntimeError(f"ANGLE build output missing: {source}")
    shutil.copyfile(source, destination / filename)
manifest = Path(__file__).resolve().parents[1] / "platform/windows/aura.manifest"
mt = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else "mt.exe"
subprocess.run([mt, "-nologo", "-manifest", str(manifest), f"-outputresource:{executable};#1"], check=True)
