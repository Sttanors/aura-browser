# Native Linux packages

The release publishes runtime/installable binaries and their checksums. GitHub already provides source
ZIP/tar.gz snapshots. Logo SVG/PNG/ICO files belong in the repository and runtime payload, not as separate
release downloads. The obsolete hand-maintained source-archive option was removed from `scripts/package.py`.

## ABI baseline and layout

The original development machine uses a newer glibc/libstdc++ ABI than Debian 13. Repacking that binary
would produce a DEB which cannot run on Debian. The native runtime is rebuilt inside Debian 13,
including Skia and the C++ application, and tested before packaging. Its runtime library path is `$ORIGIN`,
with no build-machine path. Debian 13+, Fedora 44 and current Arch/CachyOS x86_64 are the targets.

`/opt/aura-browser` contains the private CEF runtime and assets; `/usr/bin/aura-browser` chooses the
current user's XDG data directory and passes it as the profile. Desktop/icon/license entries live in
`/usr/share`. Package managers own installed files as root; profiles remain user-owned outside the package.
No installer scripts change the default browser or delete user data. No setuid sandbox or sandbox-disable
flag is introduced. The application keeps its normal Chromium sandbox and software-rendering default.

The native launcher uses the same `aura-browser-profile` as the per-user `.run` installer. Before switching
installation methods, close Aura and run `~/.local/bin/aura-browser-uninstall`; this retains the profile and
removes the old user launcher and desktop entry that would shadow `/usr/bin` and `/usr/share/applications`.

## Build

Use the normal verified bootstrap to fetch dependencies (`python3 scripts/bootstrap.py --fetch-only`).
Requires Docker and a Rust 1.88+ toolchain managed by rustup on the host. Container compilation uses four
CPU workers and a 6 GiB memory limit, with source and host Rust toolchain mounted read-only. Build outputs
live under `artifacts/native-build-root`; release packages live under `artifacts/native-packages`.
`packaging/linux/native/Dockerfile.debian` installs compiler/package tooling only inside the container.

Run `bash packaging/linux/native/build.sh --test` to build and check all three package managers.
Without `--test`, it only builds the packages.

Inside the build container, `packaging/linux/native/build-debian.sh` creates a separate source/build tree,
reuses the verified dependency sources and performs Skia, CMake and CTest builds. Then:

```sh
python3 scripts/build_native_linux.py --archive artifacts/native-packages/aura2-linux64-debian13.zip
```

The packaging command must run in the Debian packaging image (dpkg-dev, rpm, libarchive-tools and
desktop-file-utils available). It first launches the staged runtime with `--help` to catch ABI/dependency
failures without opening a window. DEB dependencies come from `dpkg-shlibdeps` over the actual ELF files;
RPM uses automatic ELF dependency generation while excluding the bundled private CEF/Vulkan libraries.
Arch declares distribution package names in `.PKGINFO` and includes an mtree manifest. Arch gzip archives
are intentional to match the requested `.pkg.tar.gz` format. Licensing metadata does not invent a new
license for Aura; third-party notices remain bundled.

Format references: [Debian shared-library policy](https://www.debian.org/doc/debian-policy/ch-sharedlibs.html),
[Arch PKGINFO](https://man.archlinux.org/man/PKGINFO.5.en), [RPM spec format](https://rpm.org/docs/6.0.x/manual/spec.html).

## Verification

`test-installed.sh` refuses to run outside a marked disposable Docker container. Each test installs through
its real package manager with dependencies enabled, launches the installed binary as an ordinary user,
validates the desktop/icon entries and root ownership, reinstalls, removes the package and verifies a profile
marker survives. The Debian-built browser also runs the existing isolated Weston/Pixman + Xwayland SHM GUI
suite on the host through `AURA_TEST_BINARY`; the real desktop display is never used.
