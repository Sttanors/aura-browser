"""Package CEF's sandbox bootstrap and privately built ANGLE without modifying the SDK."""
from brand_windows import brand
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

destination, cef, skia = map(Path, sys.argv[1:4])
executable = destination / ((sys.argv[5] if len(sys.argv) > 5 else "aura_diagnostic") + ".exe")
shutil.copyfile(cef / "Release/bootstrap.exe", executable)
for filename in (() if executable.stem == "aura_browser" else ("libEGL.dll", "libGLESv2.dll")):
    source = skia / filename
    if not source.is_file():
        raise RuntimeError(f"ANGLE build output missing: {source}")
    shutil.copyfile(source, destination / filename)
if executable.stem == "aura_browser":
    brand(executable)
    brand(destination / "aura_browser.dll")
manifest = Path(__file__).resolve().parents[1] / "platform/windows/aura.manifest"
mt = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else "mt.exe"
subprocess.run([mt, "-nologo", "-manifest", str(manifest), f"-outputresource:{executable};#1"], check=True)

# Verify the actual PE resource, not just the manifest tool's return code.
with tempfile.TemporaryDirectory(prefix="aura-manifest-") as temporary:
    extracted = Path(temporary) / "embedded.manifest"
    subprocess.run([mt, "-nologo", f"-inputresource:{executable};#1", f"-out:{extracted}"], check=True)
    tree = ET.parse(extracted)
    dpi = tree.find(".//{http://schemas.microsoft.com/SMI/2016/WindowsSettings}dpiAwareness")
    if dpi is None or dpi.text != "PerMonitorV2":
        raise RuntimeError("The browser bootstrap is missing its per-monitor DPI manifest")
