# Installer packaging

Preview.3 rebuilds the browser with Aura branding in the native window, UI and Windows PE resources.
The installers package that freshly tested runtime. Chromium, profiles and extension compatibility are unchanged.

## Linux

```bash
python3 scripts/build_linux_installer.py --archive artifacts/aura2-linux64.zip
python3 -m unittest discover -s tests/python -p test_installers.py -v
```

The offline `.run` contains the complete runtime in a gzip-compressed tar after a Bash header.
The header verifies the embedded payload's SHA-256 before unpacking. It creates a per-user launcher,
an XDG desktop entry and a separate uninstall command. SHA-256 is also published for the complete installer.
The installer refuses to overwrite an unmanaged application directory or launcher.
Profiles live outside the application directory and survive reinstall/removal. Close Aura before reinstalling.
A failed application-directory move restores the previous directory. System packages and the default browser
are not changed. Requires Linux x86_64, Bash, tar, gzip and sha256sum.

Tests use a temporary HOME/XDG_DATA_HOME, exercise Unicode and spaces in paths, invoke a fixture launcher,
validate the desktop entry, reinstall and uninstall while retaining a profile, and reject corrupt payloads
and an unmanaged target. The actual release payload is also installed in a temporary HOME, compared byte-for-byte
with the tested binary and launched with `--help`; this does not open a desktop window.

## Windows

Definition: `packaging/windows/aura.iss`, compiled with [Inno Setup 6](https://jrsoftware.org/isinfo.php).
It installs for the current user, creates Start menu shortcuts and an optional desktop shortcut,
and registers its standard uninstaller. Browser shortcuts explicitly pass a profile in LOCALAPPDATA,
outside the program directory. No profile-delete action or default-browser change is registered.
Installer UI languages: English and Russian.

To build from an extracted portable runtime in PowerShell:

```powershell
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" `
  "/DPayloadDir=C:\path\to\Aura2" "/DOutputDir=C:\output" packaging/windows/aura.iss
```

The main `Windows browser` CI packages its freshly built portable ZIP, builds the installer,
and verifies Start menu/desktop shortcut targets, real installed-browser GUI behavior,
reinstall/uninstall and profile preservation. It also verifies the executable product identity,
window icons and absence of the unused CEF bootstrap. `Preview installers` is a manual workflow
for repackaging the published preview.3 archive; it checks the downloaded SHA-256 first.

Portable archives remain available alongside installers. Existing portable profiles are not automatically migrated.
Neither installer is signed or provides an updater.
