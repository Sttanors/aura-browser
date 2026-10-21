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
| Unicode File API, Canvas 2D, WebCrypto AES-GCM, Worker | Passed | Passed in portable/installed CI |
| IndexedDB persistence after restart | Passed | Passed in portable/installed CI |
| IndexedDB isolation in private context | Passed | Native private windows covered; API journey Linux only |
| Cache API and Service Worker response without origin fetch | Passed | Passed in portable/installed CI |
| CSP inline-script denial with allowed external script | Passed | Passed in portable/installed CI |
| CORS allowed fetch / denied foreign fetch | Passed | Passed in portable/installed CI |
| Foreign iframe DOM access denied | Passed | Passed in portable/installed CI |
| Untrusted TLS certificate blocked | Passed | Full certificate fixture not repeated |
| Actual VP9+Opus WebM playback | Passed | Passed in portable/installed CI |
| Popup block policy | Passed | Passed in portable/installed CI |
| H.264 / HEVC media capability probe | Not available | Not available in CI |
| Widevine EME capability probe | Not available | Not available in CI |
| WebGL context probe in default settings | Not available | Context creation succeeded; full rendering unverified |

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

Distribution checks passed in disposable Debian 13, Arch and Fedora 44 containers:
actual runtime/package versions, dependencies, launcher/icons, reinstall/removal and profile retention.
The actual Debian runtime also passed the API/security/media/private/restart/public-page journey
and all 11 pixel comparisons. [Windows CI 39602429743](https://github.com/Sttanors/aura-browser/actions/runs/39602429743)
passed its portable and installed API journeys, exact PE/DisplayVersion checks and installer lifecycle.

A real, signature/hash-verified `0.2.0-preview.3.1` Linux `.run` upgraded to
`0.3.0-preview.1` in an isolated home containing spaces, Cyrillic and shell-special
characters. The actual launcher/GUI retained bookmarks/history/settings/cookies/
localStorage; uninstall preserved the profile. The installed executable matched
the new runtime archive exactly. This is a manual installation upgrade, since
the previous published release did not contain the new updater.

Final publication/signature verification is tracked in `browser-roadmap-progress.md`.
