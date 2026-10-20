# Product identity audit — preview.3

Scope: user-facing browser UI, native windows, application-menu/taskbar entries, executable properties,
installers, portable/source archives and public documentation. This audit does not claim an exhaustive
functional or security review of the browser engine.

| Finding | Fix |
|---|---|
| No master logo; sidebar/new-tab used nested square placeholders | SVG master, native vector drawing from the same paths |
| Linux launcher used the distribution's generic web-browser icon | Packaged Aura PNG; desktop entry references the installed file |
| X11 window had no EWMH icon and launcher lacked window-class identity | Embedded `_NET_WM_ICON`; desktop `StartupWMClass=Aura2` matches existing WM_CLASS |
| Windows window class had no icons | Large/small icons loaded from the branded EXE resources |
| CEF bootstrap inherited SDK identity in Explorer/file properties | Aura icon group and version/product resources embedded while retaining sandbox/manifest |
| Setup used a default installer icon | Multi-size Aura ICO used for setup; shortcut/uninstall use the branded application |
| Portable ZIP included the unused original `bootstrap.exe` | Excluded; only the intended branded launch executable is shipped |
| Source/runtime packages did not include branding assets | SVG/PNG/ICO included; generated native data ships with source |
| Installer-only workflow repackaged an older runtime | Main build now compiles and tests installer using that build's ZIP |
| Packaging documentation described older release inputs | Updated both language READMEs and installer documentation |

Assets: `resources/branding/aura.svg` is the master; `scripts/generate_branding.py` regenerates PNG,
7-size ICO, native vector path data and the 64px X11 ARGB icon. Builds use checked-in generated files,
so image tooling is needed only when changing the logo. No platform-specific alternate logo is used.
The old glow/square decoration was removed from the new-tab logo rather than layered behind the new mark.

Checks: Linux CTest/Python suites, isolated software-rendered browser integration with X11 icon/class
assertions, desktop-entry validation and install/reinstall/uninstall/profile preservation; Windows CI
checks PE product metadata, native large/small icons, real browser and installed-browser integration.
Historical M0 names remain confined to the separate diagnostic harness. Website globe/favicons remain
website indicators and are not substitutes for the application logo. No website user-agent spoofing was added.
