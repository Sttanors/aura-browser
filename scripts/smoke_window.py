"""Exercise Aura's Linux desktop window, not the headless capture path."""
from __future__ import annotations
import json
import ctypes
import os
from pathlib import Path
import shutil
import signal
import subprocess
import threading
import time
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler


class ButtonEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
        ("display", ctypes.c_void_p), ("window", ctypes.c_ulong), ("root", ctypes.c_ulong),
        ("subwindow", ctypes.c_ulong), ("time", ctypes.c_ulong), ("x", ctypes.c_int),
        ("y", ctypes.c_int), ("x_root", ctypes.c_int), ("y_root", ctypes.c_int),
        ("state", ctypes.c_uint), ("button", ctypes.c_uint), ("same_screen", ctypes.c_int)]


class Event(ctypes.Union):
    _fields_ = [("button", ButtonEvent), ("pad", ctypes.c_long * 24)]


def click_at(window, x, y, button=1):
    """Send window-local events; avoid moving the user's global pointer under Wayland."""
    x11 = ctypes.CDLL("libX11.so.6")
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.POINTER(Event)]
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(None)
    if not display: raise RuntimeError("Cannot open QA display")
    try:
        for kind, mask, state in ((4, 4, 0), (5, 8, 1 << (7 + button))):
            event = Event()
            event.button = ButtonEvent(type=kind, display=display, window=int(window),
                x=x, y=y, state=state, button=button, same_screen=True)
            if not x11.XSendEvent(display, int(window), False, mask, ctypes.byref(event)):
                raise RuntimeError("Cannot send QA click")
        x11.XSync(display, False)
    finally:
        x11.XCloseDisplay(display)


def drag_between(window, x1, y1, x2, y2):
    """Window-local native drag on the isolated QA display."""
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY') != '1':
        raise RuntimeError('Dragging requires the isolated QA display')
    x11 = ctypes.CDLL("libX11.so.6")
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.POINTER(Event)]
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(None)
    if not display:
        raise RuntimeError('Cannot open drag display')
    try:
        events = [(4, 4, x1, y1, 0)]
        events += [(6, 64, x1 + (x2-x1)*i//10, y1 + (y2-y1)*i//10, 256) for i in range(1, 11)]
        events += [(5, 8, x2, y2, 256)]
        for kind, mask, x, y, state in events:
            event = Event()
            event.button = ButtonEvent(type=kind, display=display, window=int(window), x=x, y=y,
                                       state=state, button=1 if kind != 6 else 0, same_screen=True)
            if not x11.XSendEvent(display, int(window), False, mask, ctypes.byref(event)):
                raise RuntimeError('Cannot send drag event')
            x11.XSync(display, False)
            time.sleep(.03)
    finally:
        x11.XCloseDisplay(display)


def key_at(window, name, state=0):
    """Send a known group-zero key without changing the Wayland keyboard layout."""
    x11 = ctypes.CDLL("libX11.so.6")
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
    x11.XStringToKeysym.restype = ctypes.c_ulong
    x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XKeysymToKeycode.restype = ctypes.c_ubyte
    x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.POINTER(Event)]
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(None)
    if not display: raise RuntimeError("Cannot open QA keyboard display")
    try:
        code = x11.XKeysymToKeycode(display, x11.XStringToKeysym(name.encode()))
        if not code: raise RuntimeError(f"No native key for {name}")
        # XKeyEvent and XButtonEvent share this layout through the detail field.
        for kind, mask in ((2, 1), (3, 2)):
            event = Event()
            event.button = ButtonEvent(type=kind, display=display, window=int(window),
                state=state, button=code, same_screen=True)
            x11.XSendEvent(display, int(window), False, mask, ctypes.byref(event))
        x11.XSync(display, False)
    finally:
        x11.XCloseDisplay(display)
    time.sleep(0.025)


def type_at(window, value):
    for character in value:
        name = "minus" if character in "-_" else character.lower()
        key_at(window, name, 1 if character.isupper() or character == "_" else 0)


def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY') != '1':
        raise RuntimeError('GUI tests require an isolated software display; the real desktop is not a test target')
    for tool in ("xdotool", "import"):
        if not shutil.which(tool): raise RuntimeError(f"Window QA requires {tool}")
    output = ROOT / "artifacts/linux-window"
    output.mkdir(parents=True, exist_ok=True)
    (output / "report.json").unlink(missing_ok=True)
    (output / "data/aura.log").unlink(missing_ok=True)
    binary = ROOT / "build/linux-x64-release/aura_diagnostic"
    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(QuietHandler, directory=str(ROOT / "tests/fixtures")))
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    with (output / "process.log").open("w") as log:
        process = subprocess.Popen([str(binary), f"--url=http://127.0.0.1:{server.server_port}/surface.html",
            f"--data-dir={output / 'data'}", f"--report={output / 'report.json'}"],
            cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
            env={**os.environ, "XMODIFIERS": "@im=none"})
        try:
            window = None
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                if process.poll() is not None: raise RuntimeError(f"Aura exited {process.returncode}; see {output / 'process.log'}")
                found = subprocess.run(["xdotool", "search", "--onlyvisible", "--pid", str(process.pid)],
                                       capture_output=True, text=True)
                if found.returncode == 0:
                    window = found.stdout.strip().splitlines()[0]
                    break
                time.sleep(0.1)
            if not window: raise RuntimeError("No mapped Aura desktop window appeared")
            def xdo(*arguments):
                subprocess.run(["xdotool", *map(str, arguments)], check=True, timeout=5)
            xdo("windowactivate", "--sync", window)
            xdo("windowfocus", "--sync", window)
            time.sleep(2)
            print(subprocess.check_output(["xdotool", "getwindowgeometry", "--shell", window], text=True), flush=True)
            subprocess.run(["import", "-window", window, str(output / "initial.png")], check=True, timeout=10)
            click_at(window, 500, 515)
            type_at(window, "AURA-linux")
            key_at(window, "a", 4)
            type_at(window, "Linux_OK")
            time.sleep(0.4)
            key_at(window, "F6")
            key_at(window, "F7")
            xdo("windowsize", window, 1100, 780)
            time.sleep(0.8)
            subprocess.run(["import", "-window", window, str(output / "window.png")], check=True, timeout=10)
            click_at(window, 1072, 32)
            process.wait(timeout=10)
            if process.returncode: raise RuntimeError(f"Window process failed: {process.returncode}")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try: process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
            server.shutdown()
            worker.join()
            server.server_close()
    report = json.loads((output / "report.json").read_text())
    if not report["loaded"] or report["failed"] or report["renderer_crashes"]:
        raise RuntimeError(f"Invalid browser state: {report}")
    if report["title"] != "Chromium OSR / input: Linux_OK":
        raise RuntimeError(f"Mouse, keyboard, or Ctrl+A failed: {report['title']}")
    if report["theme"] != "ember-light": raise RuntimeError("F6/F7 theme switching failed")
    if report["pixel_width"] != 728 or report["pixel_height"] != 586:
        raise RuntimeError(f"Resize did not update CEF view: {report['pixel_width']}x{report['pixel_height']}")
    if report["presentation_frames"] < 5 or not report["before_close_received"]:
        raise RuntimeError("Window presentation or lifecycle failed")
    print(json.dumps(report, indent=2))
    print(f"Native window screenshot: {output / 'window.png'}")


if __name__ == "__main__": main()
