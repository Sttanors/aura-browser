"""Validate native payload layout without installing into the host system."""
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))
from build_native_linux import stage_runtime


@unittest.skipUnless(sys.platform=='linux','Linux package layout')
class NativePayloadTests(unittest.TestCase):
    def test_installs_icon_launcher_and_private_runtime_without_user_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);archive=root/'runtime.zip'
            with zipfile.ZipFile(archive,'w') as z:
                for name in ['aura_browser','libcef.so','branding/aura.png','branding/aura.svg']:
                    z.writestr('Aura2/'+name,b'fixture')
            stage=root/'stage'
            stage_runtime(archive,stage)
            self.assertEqual((stage/'usr/bin/aura-browser').stat().st_mode & 0o777,0o755)
            self.assertEqual((stage/'opt/aura-browser/aura_browser').stat().st_mode & 0o777,0o755)
            desktop=(stage/'usr/share/applications/aura-browser.desktop').read_text()
            self.assertIn('Icon=/opt/aura-browser/branding/aura.png',desktop)
            self.assertIn('StartupWMClass=Aura2',desktop)
            self.assertTrue((stage/'usr/share/icons/hicolor/scalable/apps/aura-browser.svg').exists())
            self.assertFalse((stage/'home').exists())

    def test_rejects_escape_and_profiles(self):
        for name in ['Aura2/../../escaped','Aura2/profile/cookies']:
            with self.subTest(name=name),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);archive=root/'bad.zip'
                with zipfile.ZipFile(archive,'w') as z:
                    for base in ['aura_browser','libcef.so','branding/aura.png','branding/aura.svg']:
                        z.writestr('Aura2/'+base,b'fixture')
                    z.writestr(name,b'private')
                with self.assertRaises(ValueError): stage_runtime(archive,root/'stage')
                self.assertFalse((root/'escaped').exists())

@unittest.skipUnless(sys.platform=='linux' and __import__('os').geteuid()!=0,'per-user migration')
class LauncherMigrationTests(unittest.TestCase):
    def test_managed_override_is_backed_up_once_and_profile_is_kept(self):
        import os,subprocess
        script=Path(__file__).resolve().parents[2]/'packaging/linux/native/migrate-launchers'
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);home=root/'Дом с пробелами';home.mkdir()
            data=home/'Data $cash %25';system=root/'system'
            (system/'usr/bin').mkdir(parents=True)
            (system/'usr/share/applications').mkdir(parents=True)
            binary=system/'usr/bin/aura-browser';binary.write_text('#!/bin/sh\n');binary.chmod(0o755)
            (system/'usr/share/applications/aura-browser.desktop').write_text('[Desktop Entry]\n')
            paths=[data/'applications/aura-browser.desktop',home/'.local/bin/aura-browser',home/'.local/bin/aura-browser-uninstall']
            for path in paths:path.parent.mkdir(parents=True,exist_ok=True);path.write_text('# Aura installer managed\nOLD\n')
            profile=data/'aura-browser-profile';profile.mkdir();(profile/'keep').write_text('keep')
            app=data/'aura-browser';app.mkdir();(app/'keep').write_text('old runtime')
            env=dict(os.environ,HOME=str(home),XDG_DATA_HOME=str(data))
            subprocess.run(['sh',str(script),'--system-root',str(system)],env=env,check=True,capture_output=True)
            self.assertTrue(all(not path.exists() for path in paths))
            backups=list((data/'aura-browser-launcher-backups').iterdir());self.assertEqual(len(backups),1)
            self.assertEqual((backups[0]/'applications/aura-browser.desktop').read_text(),'# Aura installer managed\nOLD\n')
            self.assertEqual((backups[0]/'bin/aura-browser').read_text(),'# Aura installer managed\nOLD\n')
            self.assertEqual((profile/'keep').read_text(),'keep');self.assertEqual((app/'keep').read_text(),'old runtime')
            subprocess.run(['sh',str(script),'--system-root',str(system)],env=env,check=True,capture_output=True)
            self.assertEqual(len(list((data/'aura-browser-launcher-backups').iterdir())),1)
            # Symlink/unmanaged overrides are not touched.
            target=root/'unrelated';target.write_text('# Aura installer managed\nDo not move\n')
            paths[0].symlink_to(target)
            subprocess.run(['sh',str(script),'--system-root',str(system)],env=env,check=True,capture_output=True)
            self.assertTrue(paths[0].is_symlink());self.assertTrue(target.exists())
            paths[0].unlink();paths[0].write_text('unmanaged')
            subprocess.run(['sh',str(script),'--system-root',str(system)],env=env,check=True,capture_output=True)
            self.assertEqual(paths[0].read_text(),'unmanaged')


if __name__ == '__main__':
    unittest.main()
