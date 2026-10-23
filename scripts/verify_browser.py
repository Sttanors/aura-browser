"""Run GUI coverage on a private CPU-only Wayland/X11 display, never the desktop."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dependencies import ROOT


def stop(process):
    if process is None or process.poll() is not None:
        return
    os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=4)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', choices=('browser', 'hotkeys', 'screenshots', 'sessions', 'bookmarks', 'site-data', 'permissions', 'translation-online', 'translation', 'reader', 'commands', 'profiles', 'containers', 'split', 'sleep', 'groups', 'workspaces', 'popups', 'navigation', 'clipboard', 'settings', 'features', 'startpage', 'wallpaper', 'interface', 'scrolling', 'updates', 'update-install', 'import', 'passwords', 'extensions', 'windows', 'filters', 'rendering', 'compatibility', 'upgrade', 'all'), default='browser')
    parser.add_argument("--refresh", type=int, choices=(60, 75, 120, 144, 165, 240), default=60)
    args = parser.parse_args()
    for tool in ('weston', 'Xwayland', 'xdotool', 'import'):
        if not shutil.which(tool):
            raise RuntimeError(f'Isolated GUI verification requires {tool}; no desktop test was started')
    if args.suite in ('hotkeys', 'screenshots', 'bookmarks', 'reader', 'commands', 'settings', 'features', 'startpage', 'wallpaper', 'updates', 'update-install', 'import', 'passwords', 'extensions', 'windows', 'filters', 'rendering', 'compatibility', 'upgrade', 'all') and not shutil.which('xclip'):
        raise RuntimeError('GUI verification requires xclip for UTF-8 clipboard input')
    if args.suite in ('screenshots', 'bookmarks', 'wallpaper', 'import', 'filters', 'all') and not shutil.which('zenity'):
        raise RuntimeError('Native file chooser verification requires zenity; no desktop test was started')
    output = ROOT / 'artifacts/browser-smoke'
    output.mkdir(parents=True, exist_ok=True)
    weston = xwayland = None
    with tempfile.TemporaryDirectory(prefix='aura-software-display-') as temporary:
        runtime = Path(temporary)
        env = {**os.environ, 'XDG_RUNTIME_DIR': str(runtime), 'LIBGL_ALWAYS_SOFTWARE': '1',
            'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/nonexistent/aura-test-bus',
            'XDG_CURRENT_DESKTOP': '', 'AURA_TEST_ISOLATED_DISPLAY': '1', 'AURA_TEST_REFRESH': str(args.refresh)}
        # Fake-HOME installer tests must not detect packages on the developer host.
        env['AURA_INSTALL_SYSTEM_ROOT'] = str(runtime / 'empty-system')
        env.pop('DISPLAY', None)
        env.pop('WAYLAND_DISPLAY', None)
        with (output / 'weston-process.log').open('w') as weston_log, (output / 'xwayland.log').open('w') as x_log:
            try:
                weston = subprocess.Popen(['weston', '--backend=headless', '--renderer=pixman',
                    '--shell=kiosk-shell.so', '--width=2400', '--height=1600', f'--refresh-rate={args.refresh*1000}', '--fake-seat',
                    '--socket=wayland-aura-test', '--idle-time=0', '--no-config',
                    f'--log={output / "weston.log"}'], env=env, stdout=weston_log,
                    stderr=subprocess.STDOUT, start_new_session=True)
                deadline = time.monotonic() + 10
                while not (runtime / 'wayland-aura-test').exists():
                    if weston.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('Private software compositor did not start; see weston.log')
                    time.sleep(.05)
                env['WAYLAND_DISPLAY'] = 'wayland-aura-test'
                read_fd, write_fd = os.pipe()
                try:
                    # -shm avoids Glamor/DRM; GLX is disabled as an additional boundary.
                    xwayland = subprocess.Popen(['Xwayland', '-shm', '-extension', 'GLX',
                        '-geometry', '1600x1000', '-nolisten', 'tcp', '-noreset',
                        '-displayfd', str(write_fd)], env=env, stdout=x_log,
                        stderr=subprocess.STDOUT, pass_fds=(write_fd,), start_new_session=True)
                    os.close(write_fd)
                    write_fd = -1
                    if not select.select([read_fd], [], [], 10)[0]:
                        raise RuntimeError('Private software X11 server did not start; see xwayland.log')
                    number = os.read(read_fd, 64).decode().strip()
                    if not number.isdigit():
                        raise RuntimeError(f'Invalid private X11 display number: {number!r}')
                    env['DISPLAY'] = ':' + number
                finally:
                    os.close(read_fd)
                    if write_fd >= 0:
                        os.close(write_fd)
                print(f'Isolated display {env["DISPLAY"]}: Weston Pixman + Xwayland SHM, GLX disabled', flush=True)
                scripts = {'hotkeys': ['smoke_hotkeys.py'], 'screenshots': ['smoke_screenshot.py'], 'sessions': ['smoke_sessions.py'], 'bookmarks': ['smoke_bookmarks.py'], 'site-data': ['smoke_site_data.py'], 'permissions': ['smoke_permissions.py'], 'browser': ['smoke_browser.py'], 'translation-online': ['smoke_translation_online.py'], 'translation': ['smoke_translation.py'], 'reader': ['smoke_reader.py'], 'commands': ['smoke_commands.py'], 'profiles': ['smoke_profiles.py'], 'workspaces': ['smoke_workspaces.py'], 'groups': ['smoke_groups.py'], 'sleep': ['smoke_sleep.py'], 'split': ['smoke_split.py'], 'containers': ['smoke_containers.py'], 'popups': ['smoke_popups.py'], 'navigation': ['smoke_navigation.py'], 'clipboard': ['smoke_clipboard.py'], 'settings': ['smoke_settings.py'],
                           'features': ['smoke_features.py'], 'startpage': ['smoke_startpage.py'], 'wallpaper': ['smoke_wallpaper.py'], 'scrolling': ['smoke_scrolling.py'], 'interface': ['smoke_interface.py'], 'updates': ['smoke_updates.py'], 'update-install': ['smoke_update_install.py'], 'import': ['smoke_import.py'], 'passwords': ['smoke_passwords.py'], 'extensions': ['smoke_extension_lifecycle.py'], 'windows': ['smoke_windows.py'], 'filters': ['smoke_filters.py'], 'rendering': ['smoke_rendering.py'], 'compatibility': ['smoke_compatibility.py'], 'upgrade': ['smoke_upgrade.py'], 'all': ['smoke_hotkeys.py', 'smoke_screenshot.py', 'smoke_sessions.py', 'smoke_bookmarks.py', 'smoke_site_data.py', 'smoke_permissions.py', 'smoke_translation.py', 'smoke_reader.py', 'smoke_commands.py', 'smoke_profiles.py', 'smoke_containers.py', 'smoke_split.py', 'smoke_workspaces.py', 'smoke_groups.py', 'smoke_sleep.py', 'smoke_popups.py', 'smoke_navigation.py', 'smoke_clipboard.py', 'smoke_compatibility.py', 'smoke_rendering.py', 'smoke_scrolling.py', 'smoke_interface.py', 'smoke_browser.py', 'smoke_settings.py', 'smoke_startpage.py', 'smoke_wallpaper.py', 'smoke_features.py', 'smoke_updates.py', 'smoke_import.py', 'smoke_passwords.py', 'smoke_extension_lifecycle.py', 'smoke_windows.py', 'smoke_filters.py']}[args.suite]
                for script in scripts:
                    subprocess.run([sys.executable, str(ROOT / 'scripts' / script)],
                        cwd=ROOT, env=env, check=True, timeout=600 if args.suite in ('sleep','update-install','extensions','filters','compatibility','upgrade') or script in ('smoke_sleep.py','smoke_extension_lifecycle.py','smoke_filters.py','smoke_compatibility.py') else 240)
            finally:
                stop(xwayland)
                stop(weston)


if __name__ == '__main__':
    main()
