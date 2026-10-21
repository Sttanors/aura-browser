"""Release metadata must agree across compiled identity and package formats."""
from pathlib import Path
import struct,sys,tempfile,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))
from release_identity import ReleaseIdentity,read_identity
from brand_windows import version_resource

class ReleaseIdentityTests(unittest.TestCase):
    def test_preview_and_stable_package_versions(self):
        preview=ReleaseIdentity('1.2.3-preview.4',9)
        self.assertEqual(preview.numeric_version,[1,2,3,9])
        self.assertEqual(preview.native_version,'1.2.3~preview.4')
        self.assertEqual(preview.arch_version,'1.2.3rc4')
        self.assertEqual(preview.package_name('debian'),'aura-browser_1.2.3-preview.4-1_amd64.deb')
        self.assertEqual(preview.package_name('fedora'),'aura-browser-1.2.3-preview.4-1.x86_64.rpm')
        self.assertEqual(preview.package_name('arch'),'aura-browser-1.2.3rc4-1-x86_64.pkg.tar.gz')
        stable=ReleaseIdentity('1.2.3',10)
        self.assertEqual(stable.native_version,'1.2.3')
        self.assertEqual(stable.arch_version,'1.2.3')
    def test_rejects_unsafe_and_out_of_range_identity(self):
        for version,seq in [('1.2.3/../../x',1),('1.2',1),('01.2.3',1),('1.2.3-preview.',1),('1.2.3-preview.0',1),('65536.2.3',1),('1.2.3',0),('1.2.3',65536)]:
            with self.subTest(version=version,seq=seq),self.assertRaises(ValueError):ReleaseIdentity(version,seq)
    def test_pe_resources_contain_exact_numeric_and_product_identity(self):
        identity=ReleaseIdentity('1.2.3-preview.4',9)
        for dll in (False,True):
            resource=version_resource(dll,identity)
            start=resource.index(struct.pack('<I',0xFEEF04BD))
            fixed=struct.unpack_from('<13I',resource,start)
            self.assertEqual(fixed[2:6],(0x10002,0x30009,0x10002,0x30009))
            self.assertEqual(fixed[9],2 if dll else 1)
            self.assertIn('1.2.3-preview.4'.encode('utf-16le'),resource)
            self.assertIn('1.2.3.9'.encode('utf-16le'),resource)
    def test_reader_rejects_missing_and_ambiguous_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'common').mkdir();header=root/'common/update_key.hpp'
            for text in ('', 'inline constexpr std::string_view browser_version = "1.2.3";'):
                header.write_text(text)
                with self.assertRaises(ValueError):read_identity(root)
            valid='inline constexpr std::string_view browser_version = "1.2.3";\ninline constexpr long long browser_release_sequence = 9;\n'
            header.write_text(valid);self.assertEqual(read_identity(root).version,'1.2.3')
            header.write_text(valid+valid)
            with self.assertRaises(ValueError):read_identity(root)
    def test_current_compiled_identity_is_accepted(self):
        self.assertEqual(read_identity().as_dict()['tag'],'v'+read_identity().version)
if __name__=='__main__':unittest.main()
