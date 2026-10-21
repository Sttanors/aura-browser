#!/bin/sh
# Runs only inside a disposable container. Never pass host / or home directories.
set -eu
[ "${AURA_PACKAGE_TEST_CONTAINER:-}" = 1 ] && [ -f /.dockerenv ] || { echo "Run only in the disposable package-test container." >&2; exit 1; }
family=$1
package=$2
case $family in
    debian)
        apt-get update
        apt-get install -y --no-install-recommends "$package" desktop-file-utils
        ;;
    fedora)
        dnf install -y --setopt=install_weak_deps=False "$package" shadow-utils desktop-file-utils util-linux
        ;;
    arch)
        pacman -Syu --noconfirm
        pacman -S --needed --noconfirm desktop-file-utils
        pacman -U --noconfirm "$package"
        ;;
    *) exit 2 ;;
esac
useradd --create-home --home-dir '/home/Aura user' aura-test
runuser -u aura-test -- aura-browser --help
actual_version=$(runuser -u aura-test -- aura-browser --version)
test "$actual_version" = "$3"
case $family in
    debian) package_version=$(dpkg-query -W -f='${Version}' aura-browser) ;;
    fedora) package_version=$(rpm -q --qf '%{VERSION}-%{RELEASE}' aura-browser) ;;
    arch) package_version=$(pacman -Q aura-browser); package_version=${package_version#* } ;;
esac
test "$package_version" = "$4"
desktop-file-validate /usr/share/applications/aura-browser.desktop
test -f /opt/aura-browser/branding/aura.png
test -f /usr/share/icons/hicolor/scalable/apps/aura-browser.svg
# Package permissions must allow only root to change installed program files.
test "$(stat -c %u /opt/aura-browser/aura_browser)" = 0
test "$(stat -c %a /opt/aura-browser/aura_browser)" = 755
profile='/home/Aura user/.local/share/aura-browser-profile'
runuser -u aura-test -- mkdir -p "$profile"
runuser -u aura-test -- sh -c 'echo keep > "$HOME/.local/share/aura-browser-profile/keep"'
case $family in
    debian) apt-get install --reinstall -y "$package"; apt-get purge -y aura-browser ;;
    fedora) dnf reinstall -y "$package"; dnf remove -y aura-browser ;;
    arch) pacman -U --noconfirm "$package"; pacman -R --noconfirm aura-browser ;;
esac
test ! -e /usr/bin/aura-browser
test ! -e /opt/aura-browser/aura_browser
test ! -e /usr/share/applications/aura-browser.desktop
test "$(cat "$profile/keep")" = keep
printf 'PASS: %s package install, dependencies, real launcher, icon, ownership, reinstall/remove, profile preservation\n' "$family"
