# Compatibility verification

This records observed behavior, not a promise of compatibility with all websites.
Current source targets Linux x86_64 and Windows x64. Native Wayland surfaces and
physical GPU rendering are not covered by the software checks.

## Browser checks

`python3 scripts/verify_browser.py --suite=compatibility` uses a private CPU-only
display and three fresh/restarted/private browser launches. The fixture contains
fictional data only; no account login, real credentials or user clipboard is used.
Public pages are loaded read-only, without an authenticated profile.

| Capability | Linux software CEF | Windows |
|---|---|---|
| Unicode File API, Canvas 2D, WebCrypto AES-GCM, Worker | Passed | CI pending |
| IndexedDB persistence after restart | Passed | CI pending |
| IndexedDB isolation in private context | Passed | Native private windows covered; API journey Linux only |
| Cache API and Service Worker response without origin fetch | Passed | CI pending |
| CSP inline-script denial with allowed external script | Passed | CI pending |
| CORS allowed fetch / denied foreign fetch | Passed | CI pending |
| Foreign iframe DOM access denied | Passed | CI pending |
| Untrusted TLS certificate blocked | Passed | Full certificate fixture not repeated |
| Actual VP9+Opus WebM playback | Passed | CI pending |
| Popup block policy | Passed | CI pending |
| H.264 / HEVC media capability probe | Not available | Probe pending |
| Widevine EME capability probe | Not available | Probe pending |
| WebGL in default software mode | Not available | Probe pending |

The 11 KiB WebM is an original synthetic fixture generated with FFmpeg's testsrc2
and sine sources, libvpx-vp9 and libopus (160×90, 10 fps, one second). It contains
no third-party film or music. Capability probes are not a substitute for actual
playback; WebM is tested by advancing playback time and checking decoded dimensions.
No DRM-protected stream or commercial codec playback is claimed.

Live Linux loads on 2026-10-21: Wikipedia home, MDN home and this GitHub repository
all returned HTTP 200 with the expected title and no load error. This verifies
initial loading, not every action/login/upload or every embedded media format.
Raw API and live-page results are in `artifacts/compatibility-smoke/`.

The broader regression suites additionally cover navigation/Unicode input,
settings/permission prompts/downloads, signed updates, imports, password vault
and guarded HTTPS fill, extension lifecycle/popups/storage/messaging/DNR,
normal/private native windows and session recovery. Physical printer behavior,
full CJK IME, screen readers, GNOME/KDE multi-monitor environments and real
120–165 Hz GPU presentation remain unverified. Chrome extension APIs dependent
on Chrome's complete tab/window model remain limited as documented separately.

## Distribution checks

A new release must use one compiled version in About, `--version`, runtime report,
Windows PE/installer/installed DisplayVersion and native package metadata.
`common/update_key.hpp` is the public identity; `scripts/release_identity.py`
reads it. Sequence also supplies the fourth numeric PE version component.
Debian/RPM use `~preview.N` internally; filenames keep `-preview.N` and Arch uses
`rcN`, so prerelease ordering and signed filename validation both remain correct.

Windows installation tests compare the actual PE identity and installed
DisplayVersion. Inno separates the numeric product version from its textual value
([official directive documentation](https://jrsoftware.org/ishelp/topic_setup_versioninfoproductversion.htm)).
Linux package tests compare real runtime `--version` and dpkg/rpm/pacman metadata,
then reinstall/remove and verify the profile survives. The Debian 13 build is the
ABI baseline; repackaging the newer development-host binary is not sufficient.
Container install checks do not imply physical desktop/GPU validation on each distro.

Installer/package results and final release verification are recorded in
`browser-roadmap-progress.md` when available. No new release is declared verified
by this document until those checks finish.
