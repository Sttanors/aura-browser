"""Run the final visual/DPI matrix and MV3 probe, preserving concise evidence."""
import argparse
import json
import os
import platform
import subprocess
import sys
from datetime import datetime, timezone
from dependencies import ROOT

parser = argparse.ArgumentParser()
parser.add_argument("--software-rendering", action="store_true")
args = parser.parse_args()
matrix = [("aurora-dark", "1"), ("aurora-light", "1"), ("ember-dark", "1.25"),
          ("ember-light", "1.5"), ("monolith-dark", "2"), ("monolith-light", "2")]
results = []
for theme, scale in matrix + [("aurora-dark", "1")]:
    extension = len(results) == len(matrix)
    command = [sys.executable, str(ROOT / "scripts/smoke.py"), "--theme", theme, "--scale", scale]
    if args.software_rendering: command.append("--software-rendering")
    if extension: command.append("--extension-probe")
    run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    suffix = theme + "-" + scale.replace('.', '_') + ("-mv3" if extension else "")
    if run.returncode:
        print(run.stdout + run.stderr, file=sys.stderr)
        sys.exit(run.returncode)
    report = json.loads((ROOT / "artifacts" / suffix / "report.json").read_text())
    if "stack smashing" in (ROOT / "artifacts" / suffix / "process.log").read_text():
        raise RuntimeError(f"A child process failed during {suffix}")
    expected_width = round(908 * float(scale))
    expected_height = round(606 * float(scale))
    if abs(report["pixel_width"] - expected_width) > 1 or abs(report["pixel_height"] - expected_height) > 1:
        raise RuntimeError(f"Incorrect OSR backing size: {suffix}")
    results.append({"case": suffix, "report": str((ROOT / "artifacts" / suffix / "report.json").relative_to(ROOT)),
                    "frames": report["paint_frames"], "passed": True})
    print(f"PASS {suffix}: {report['paint_frames']} frames", flush=True)
summary = {"milestone": "M0", "verified_at_utc": datetime.now(timezone.utc).isoformat(),
           "host": platform.platform(), "python": platform.python_version(),
           "chromium_sandbox": True, "software_rendering": args.software_rendering,
           "cases": results, "windows_native_checks": "not_executed"}
(ROOT / "artifacts/verification.json").write_text(json.dumps(summary, indent=2) + "\n")
