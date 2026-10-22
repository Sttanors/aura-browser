"""Exercise Linux installation in a temporary HOME without launching Chromium."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'scripts'))
from build_linux_installer import build


@unittest.skipUnless(sys.platform == 'linux' and os.geteuid() != 0, 'per-user Linux installer')
class LinuxInstallerTests(unittest.TestCase):
    def test_install_launch_upgrade_uninstall_preserves_profile(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            home = root/'Дом с пробелами'
            home.mkdir()
            data = home/'data $cash %value'
            archive = root/'runtime.zip'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('Aura2/aura_browser', '#!/usr/bin/env bash\nprintf "%s\\n" "$@"\n')
                z.writestr('Aura2/resources.pak', 'runtime')
                z.writestr('Aura2/branding/aura.png', 'icon')
            installer = root/'Aura setup.run'
            build(archive, installer)
            env = dict(os.environ, AURA_INSTALL_SYSTEM_ROOT=str(root/"empty-system"), HOME=str(home), XDG_DATA_HOME=str(data))
            def run(*args, **kw):
                return subprocess.run(args, env=env, check=True, text=True, capture_output=True, **kw)
            run('bash', str(installer))
            launcher = home/'.local/bin/aura-browser'
            result = run(str(launcher), '--url=https://example.com/')
            self.assertEqual(result.stdout.splitlines(), [f'--data-dir={data}/aura-browser-profile', '--url=https://example.com/'])
            desktop = data/'applications/aura-browser.desktop'
            self.assertIn(f'Icon={data}/aura-browser/branding/aura.png',desktop.read_text())
            self.assertIn('StartupWMClass=Aura2',desktop.read_text())
            if shutil.which('desktop-file-validate'):
                run('desktop-file-validate', str(desktop))
            profile = data/'aura-browser-profile'
            profile.mkdir()
            (profile/'history').write_text('keep')
            run('bash', str(installer))
            self.assertEqual((profile/'history').read_text(), 'keep')
            self.assertTrue((data/'aura-browser/resources.pak').is_file())
            run(str(home/'.local/bin/aura-browser-uninstall'))
            self.assertFalse((data/'aura-browser').exists())
            self.assertFalse(launcher.exists())
            self.assertFalse(desktop.exists())
            self.assertEqual((profile/'history').read_text(), 'keep')

    def test_rejects_corrupt_payload_and_unmanaged_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = root/'runtime.zip'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('Aura2/aura_browser', 'test')
            installer = root/'setup.run'
            build(archive, installer)
            home = root/'home'
            home.mkdir()
            env = dict(os.environ, AURA_INSTALL_SYSTEM_ROOT=str(root/"empty-system"), HOME=str(home), XDG_DATA_HOME=str(home/'data'))
            original = installer.read_bytes()
            installer.write_bytes(original[:-4]+b'bad!')
            p = subprocess.run(['bash', str(installer)], env=env, capture_output=True)
            self.assertNotEqual(p.returncode, 0)
            self.assertFalse((home/'data/aura-browser').exists())
            installer.write_bytes(original)
            app = home/'data/aura-browser'
            app.mkdir()
            (app/'unrelated').write_text('keep')
            p = subprocess.run(['bash', str(installer)], env=env, capture_output=True)
            self.assertNotEqual(p.returncode, 0)
            self.assertEqual((app/'unrelated').read_text(), 'keep')

    def test_incompatible_upgrade_preserves_working_installation_and_profile(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            home = root/'home'
            home.mkdir()
            data = home/'data'
            app = data/'aura-browser'
            app.mkdir(parents=True)
            (app/'.aura-installed').touch()
            (app/'aura_browser').write_text('previous executable')
            profile = data/'aura-browser-profile'
            profile.mkdir()
            (profile/'profile.toml').write_text('keep')
            archive = root/'bad-runtime.zip'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('Aura2/aura_browser', '#!/usr/bin/env bash\nexit 1\n')
            installer = root/'bad.run'
            build(archive, installer)
            p = subprocess.run(['bash', str(installer)], env=dict(os.environ, AURA_INSTALL_SYSTEM_ROOT=str(root/"empty-system"), HOME=str(home), XDG_DATA_HOME=str(data)),
                               capture_output=True)
            self.assertNotEqual(p.returncode, 0)
            self.assertEqual((app/'aura_browser').read_text(), 'previous executable')
            self.assertEqual((profile/'profile.toml').read_text(), 'keep')

    def test_refuses_to_shadow_existing_system_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);home=root/'home';home.mkdir()
            system=root/'system'
            (system/'usr/bin').mkdir(parents=True)
            (system/'usr/share/applications').mkdir(parents=True)
            binary=system/'usr/bin/aura-browser';binary.write_text('#!/bin/sh\n');binary.chmod(0o755)
            (system/'usr/share/applications/aura-browser.desktop').write_text('[Desktop Entry]\n')
            archive=root/'runtime.zip'
            with zipfile.ZipFile(archive,'w') as z:z.writestr('Aura2/aura_browser','unused')
            installer=root/'setup.run';build(archive,installer)
            result=subprocess.run(['bash',str(installer)],env=dict(os.environ,HOME=str(home),XDG_DATA_HOME=str(home/'data'),AURA_INSTALL_SYSTEM_ROOT=str(system)),capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('system package',result.stderr)
            self.assertFalse((home/'data/aura-browser').exists())


if __name__ == '__main__':
    unittest.main()
