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


if __name__=='__main__': unittest.main()
