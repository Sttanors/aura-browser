# Linux rendering and Wayland refresh

Aura currently renders its native window through X11/Xwayland and Skia raster.
CEF uses software OSR by default. Native Wayland surfaces and stable Linux GPU
acceleration are separate missing capabilities; reading wl_output does not make
this a native Wayland browser. The previous physical GPU freeze has not been
reproduced or declared fixed. Tests never connect to the desktop compositor/GPU.

## Current source changes

- Raster page drawing partitions the inner rectangles from the antialiased
  perimeter in device pixels. Full and page-only repaints use the same painter.
  Native controls, overlays and focus targets keep their existing behavior.
- The experimental GPU path retains its original single image draw, avoiding
  repeated full buffer copies. It was not runtime-tested on physical hardware.
- Wayland output discovery no longer performs synchronous roundtrips during
  window creation. A silent compositor connection does not block startup or
  polling; RandR/default refresh remains available until output data arrives.
- Startup selects cached or bundled filters before compilation, avoiding a
  redundant bundled-engine compilation for profiles with a valid filter cache.
- Diagnostic reports separate draw/upload/total p95 costs. These describe CPU
  painting, not renderer latency or physical input-to-display latency.

## Reproducible checks

Run `python3 scripts/verify_browser.py --suite=scrolling --refresh=165` (also 120)
and `--suite=rendering`. They create a private Weston Pixman/Xwayland SHM display
with GLX disabled. Rendering additionally uses Pillow. For before/after work,
`AURA_SCROLL_BINARY` selects a preserved executable. `AURA_SCROLL_BASELINE=1`
records measurements with scrolling assertions disabled and is not a PASS check.

In the measured 2200×1500, 125% interface, 16-wheel-event scenario at virtual
165 Hz on the development machine:

| Measurement | Preserved checkpoint 6 | Optimized source |
|---|---:|---:|
| Presentation interval p95 | 43.156 ms | 19.154 ms |
| Native frames during scenario | 38 | 61 |
| Main-process CPU time | 830 ms | 720 ms |

The final source passes the existing <25 ms p95 criterion in this scenario.
Measurements vary with CPU load and are not a guarantee of 165 frames/s on real
monitors or every page. Display/presentation samples and raw logs are tracked in
`browser-roadmap-progress.md`; renderer, compositor and physical display timing
are distinct. The two-buffer Present queue remains bounded.

Pixel comparisons after 30 page-only repaints match the full painter exactly
at all four corners for 75/80/90/100/110/125/150/175/200% interface scales,
light/dark themes and two extra device-scale 1.25 cases (11 configurations).
The test verifies that full and page-only paths were both exercised, including
continuous page animation and balanced CEF shutdown.
