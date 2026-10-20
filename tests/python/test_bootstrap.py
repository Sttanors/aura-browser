import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from dependencies import extract_archive, download, lock, archive_tree_checksum, verified
from generate_tokens import generate


class ArchiveTests(unittest.TestCase):
    def test_rejects_zip_traversal_before_writing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "bad.zip"
            with zipfile.ZipFile(archive, "w") as stream:
                stream.writestr("root/../../escaped", "bad")
            with self.assertRaises(ValueError):
                extract_archive(archive, root / "out", True)
            self.assertFalse((root / "escaped").exists())

    def test_rejects_tar_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "bad.tar.gz"
            with tarfile.open(archive, "w:gz") as stream:
                member = tarfile.TarInfo("root/link")
                member.type = tarfile.SYMTYPE
                member.linkname = "/etc/passwd"
                stream.addfile(member)
            with self.assertRaises(ValueError):
                extract_archive(archive, root / "out", True)

    def test_materializes_internal_links_without_creating_filesystem_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "links.tar.gz"
            with tarfile.open(archive, "w:gz") as stream:
                item = tarfile.TarInfo("package/source.txt")
                item.size = 2
                stream.addfile(item, io.BytesIO(b"ok"))
                link = tarfile.TarInfo("package/nested/link.txt")
                link.type = tarfile.SYMTYPE
                link.linkname = "../source.txt"
                stream.addfile(link)
            extract_archive(archive, root / "out", True)
            self.assertEqual((root/"out/nested/link.txt").read_bytes(), b"ok")
            self.assertFalse((root/"out/nested/link.txt").is_symlink())

    def test_extracts_nested_files_and_strips_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "good.tar.gz"
            with tarfile.open(archive, "w:gz") as stream:
                member = tarfile.TarInfo("package/nested/file.txt")
                member.size = 2
                stream.addfile(member, io.BytesIO(b"ok"))
            extract_archive(archive, root / "out", True)
            self.assertEqual((root / "out/nested/file.txt").read_text(), "ok")
            self.assertEqual(int((root / "out/nested/file.txt").stat().st_mtime), 0)


class ContentIntegrityTests(unittest.TestCase):
    def test_tree_digest_ignores_timestamps_but_rejects_changed_content(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = [Path(temporary)/f"{i}.tar.gz" for i in range(3)]
            for i, path in enumerate(paths):
                with tarfile.open(path, "w:gz") as archive:
                    member = tarfile.TarInfo("file.txt")
                    member.size = 2
                    member.mode = 0o644
                    member.mtime = i
                    archive.addfile(member, io.BytesIO(b"ok" if i < 2 else b"no"))
            entry = {"sha256": "0"*64, "tree_sha256": archive_tree_checksum(paths[0])}
            self.assertTrue(verified(paths[0], entry))
            self.assertTrue(verified(paths[1], entry))
            self.assertFalse(verified(paths[2], entry))
            self.assertFalse(verified(paths[0], {"sha256": "0"*64}))


class TokenTests(unittest.TestCase):
    def tokens(self):
        return json.loads((Path(__file__).resolve().parents[2] / "design/tokens.json").read_text())

    def test_generation_is_deterministic(self):
        self.assertEqual(generate(self.tokens()), generate(self.tokens()))

    def test_rejects_malformed_color(self):
        data = self.tokens()
        data["themes"][0]["accent"] = "red; arbitrary_cpp"
        with self.assertRaises(ValueError):
            generate(data)

    def test_rejects_missing_or_duplicate_theme(self):
        data = self.tokens()
        data["themes"][0] = data["themes"][1]
        with self.assertRaises(ValueError):
            generate(data)

    def test_rejects_invalid_identifier(self):
        data = self.tokens()
        data["motion"]["bad;name"] = 10
        with self.assertRaises(ValueError):
            generate(data)


if __name__ == "__main__":
    unittest.main()
