#!/usr/bin/env bash
set -euo pipefail
if [[ ${1:-} == --help ]]; then
    echo 'Aura installer: bash aura2-linux64.run'
    echo 'Installs for the current user. Requires bash, tar, gzip, sha256sum and Linux x86_64.'
    exit 0
fi
[[ $(uname -m) == x86_64 ]] || { echo 'Linux x86_64 is required.' >&2; exit 1; }
[[ $EUID != 0 ]] || { echo 'Run as your regular user, without sudo.' >&2; exit 1; }
for tool in tar gzip sha256sum; do command -v "$tool" >/dev/null; done
app_root="${XDG_DATA_HOME:-$HOME/.local/share}/aura-browser"
profile="${XDG_DATA_HOME:-$HOME/.local/share}/aura-browser-profile"
bin_root="$HOME/.local/bin"
applications="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
[[ $app_root == /* && $profile == /* ]] || { echo 'XDG_DATA_HOME must be absolute.' >&2; exit 1; }
if [[ -e $app_root && ! -f $app_root/.aura-installed ]]; then
    echo "Refusing to overwrite an unmanaged directory: $app_root" >&2; exit 1
fi
# Never replace files belonging to another application.
for owned in "$bin_root/aura-browser" "$bin_root/aura-browser-uninstall" "$applications/aura-browser.desktop"; do
    if [[ -e $owned ]] && ! grep -q 'Aura installer managed' "$owned"; then
        echo "Refusing to overwrite an unmanaged file: $owned" >&2; exit 1
    fi
done
mkdir -p "$(dirname "$app_root")" "$bin_root" "$applications"
stage=$(mktemp -d "$(dirname "$app_root")/.aura-install.XXXXXXXX")
trap 'rm -rf -- "$stage"' EXIT
line=$(awk '/^__AURA_PAYLOAD__$/ { print NR + 1; exit }' "$0")
[[ -n $line ]] || { echo 'Installer payload is missing.' >&2; exit 1; }
tail -n +"$line" "$0" > "$stage/payload.tar.gz"
printf '%s  %s\n' '@PAYLOAD_SHA256@' "$stage/payload.tar.gz" | sha256sum --check --status
mkdir "$stage/app"
tar -xzf "$stage/payload.tar.gz" -C "$stage/app" --no-same-owner
[[ -f $stage/app/Aura2/aura_browser ]] || { echo 'Incomplete payload.' >&2; exit 1; }
chmod +x "$stage/app/Aura2/aura_browser"
touch "$stage/app/Aura2/.aura-installed"
# Refuse incompatible runtimes before touching a working installation.
"$stage/app/Aura2/aura_browser" --help > "$stage/runtime-check.log" 2>&1 || {
    cat "$stage/runtime-check.log" >&2
    echo 'The new Aura runtime cannot start. The existing installation was kept.' >&2
    exit 1
}
# Profile stays outside the application, including on reinstall and uninstall.
if [[ -d $app_root ]]; then mv -- "$app_root" "$stage/previous"; fi
if ! mv -- "$stage/app/Aura2" "$app_root"; then
    [[ ! -d $stage/previous ]] || mv -- "$stage/previous" "$app_root"
    exit 1
fi
{
    echo '#!/usr/bin/env bash'
    echo '# Aura installer managed'
    printf 'exec %q %q "$@"\n' "$app_root/aura_browser" "--data-dir=$profile"
} > "$bin_root/aura-browser"
chmod +x "$bin_root/aura-browser"
{
    echo '#!/usr/bin/env bash'
    echo '# Aura installer managed'
    echo 'set -euo pipefail'
    printf 'app_root=%q\n' "$app_root"
    echo '[[ -f $app_root/.aura-installed ]] || { echo "Installation marker missing; nothing removed." >&2; exit 1; }'
    echo 'rm -rf -- "$app_root"'
    printf 'rm -f -- %q %q %q\n' "$bin_root/aura-browser" "$applications/aura-browser.desktop" "$bin_root/aura-browser-uninstall"
    printf 'echo %q\n' "Aura removed. Profile retained: $profile"
} > "$bin_root/aura-browser-uninstall"
chmod +x "$bin_root/aura-browser-uninstall"
# Desktop Entry Exec is not shell syntax. Escape its quoted argument explicitly.
desktop_exec=${bin_root//\\/\\\\}
desktop_exec=${desktop_exec//\$/\\\$}
desktop_exec=${desktop_exec//\`/\\\`}
desktop_exec=${desktop_exec//\"/\\\"}
desktop_exec=${desktop_exec//%/%%}
cat > "$applications/aura-browser.desktop" <<DESKTOP
[Desktop Entry]
# Aura installer managed
Version=1.0
Type=Application
Name=Aura Browser
Name[ru]=Браузер Aura
Comment=Browse the web with Aura
Comment[ru]=Просмотр веб-страниц в Aura
Exec="$desktop_exec/aura-browser"
Icon=$app_root/branding/aura.png
StartupWMClass=Aura2
Terminal=false
Categories=Network;WebBrowser;
DESKTOP
if command -v update-desktop-database >/dev/null; then update-desktop-database "$applications" || true; fi
printf 'Aura installed. Open Aura Browser from the applications menu.\nLauncher: %s\nUninstall: %s\n' "$bin_root/aura-browser" "$bin_root/aura-browser-uninstall"
exit 0
__AURA_PAYLOAD__
