"""Exercise a real CEF renderer against an offline HTTP fixture and capture the Skia shell."""
from __future__ import annotations
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import threading
from dependencies import ROOT


class QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--allow-unsandboxed", action="store_true")
    parser.add_argument("--software-rendering", action="store_true")
    parser.add_argument("--theme", default="aurora-dark")
    parser.add_argument("--scale", default="1")
    parser.add_argument("--extension-probe", action="store_true")
    parser.add_argument("--shared-texture-probe", action="store_true")
    args = parser.parse_args()
    platform = "windows" if os.name == "nt" else "linux"
    binary = args.binary or ROOT / f"build/{platform}-x64-release/aura_diagnostic{'.exe' if os.name == 'nt' else ''}"
    if not binary.is_file():
        parser.error(f"Build the diagnostic first: {binary}")
    suffix = args.theme + "-" + args.scale.replace('.', '_')
    if args.extension_probe: suffix += "-mv3"
    if args.shared_texture_probe: suffix += "-shared"
    output = ROOT / "artifacts" / suffix
    output.mkdir(parents=True, exist_ok=True)
    with ThreadingHTTPServer(("127.0.0.1", 0), partial(QuietHandler, directory=str(ROOT / "tests/fixtures"))) as server:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        command = [str(binary.resolve()), "--capture", f"--url=http://127.0.0.1:{server.server_port}/surface.html",
            f"--snapshot={output / 'capture.png'}", f"--report={output / 'report.json'}", f"--data-dir={output / 'data'}",
            f"--theme={args.theme}", f"--scale={args.scale}", "--timeout-ms=20000"]
        if args.allow_unsandboxed: command.append("--allow-unsandboxed")
        if args.software_rendering: command.append("--software-rendering")
        if args.extension_probe: command.append(f"--extension-probe={ROOT / 'tests/fixtures/mv3'}")
        if args.shared_texture_probe: command.append("--shared-texture-probe")
        try:
            with (output / "process.log").open("w") as log:
                result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=35)
        finally:
            server.shutdown()
            worker.join()
    if result.returncode != 0:
        raise RuntimeError(f"CEF exited {result.returncode}; inspect {output / 'process.log'}")
    report = json.loads((output / "report.json").read_text())
    required = {"loaded": True, "failed": False, "snapshot_written": True,
                "timed_out": False, "before_close_received": True, "cef_shutdown_completed": True}
    for key, expected in required.items():
        if report.get(key) != expected:
            raise RuntimeError(f"Invalid {key}: {report.get(key)}")
    if report["http_status"] != 200 or report["paint_frames"] < 3:
        raise RuntimeError("The live HTTP/JavaScript surface did not produce enough frames")
    if args.shared_texture_probe and report["accelerated_frames"] < 3:
        raise RuntimeError("No accelerated CEF frames observed")
    if args.extension_probe:
        probe = json.loads(report["mv3_probe"])
        if not all(probe.get(k) for k in ("content_script", "runtime_messaging", "service_worker", "tabs_query")):
            raise RuntimeError(f"MV3 compatibility probe failed: {probe}")
    print(json.dumps(report, indent=2))
    print(f"Capture: {output / 'capture.png'}")


if __name__ == "__main__":
    main()
