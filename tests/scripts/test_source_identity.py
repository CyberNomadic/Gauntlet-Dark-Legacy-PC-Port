"""The source identity digests what the game is built from, the same on every platform."""
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import source_identity


def tree(root: pathlib.Path, files: dict[str, bytes]) -> None:
    for relative, content in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)


class DigestTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        tree(self.root, {
            "VERSION": b"1.2.3\n",
            "src/game/app/Gauntlet.cpp": b"int main() {}\n",
            "src/engine/core/Types.h": b"#pragma once\n",
            "src/engine/net/Cargo.toml": b"[package]\n",
            "src/game/README.md": b"notes\n",
        })

    def tearDown(self):
        self.directory.cleanup()

    def test_covers_sources_and_version_in_path_order(self):
        self.assertEqual(source_identity.source_files(self.root), [
            "VERSION", "src/engine/core/Types.h", "src/game/app/Gauntlet.cpp"])

    def test_digest_is_hex_and_repeatable(self):
        first = source_identity.digest(self.root)
        self.assertEqual(len(first), 64)
        self.assertTrue(all(c in "0123456789abcdef" for c in first))
        self.assertEqual(first, source_identity.digest(self.root))

    def test_line_endings_do_not_change_the_digest(self):
        before = source_identity.digest(self.root)
        tree(self.root, {"src/game/app/Gauntlet.cpp": b"int main() {}\r\n"})
        self.assertEqual(before, source_identity.digest(self.root))

    def test_content_path_and_version_each_change_the_digest(self):
        before = source_identity.digest(self.root)
        tree(self.root, {"src/game/app/Gauntlet.cpp": b"int main() { return 1; }\n"})
        changed = source_identity.digest(self.root)
        self.assertNotEqual(before, changed)
        (self.root / "src/game/app/Gauntlet.cpp").rename(self.root / "src/game/app/Game.cpp")
        moved = source_identity.digest(self.root)
        self.assertNotEqual(changed, moved)
        tree(self.root, {"VERSION": b"1.2.4\n"})
        self.assertNotEqual(moved, source_identity.digest(self.root))

    def test_files_the_build_does_not_compile_do_not_count(self):
        before = source_identity.digest(self.root)
        tree(self.root, {"src/game/README.md": b"other notes\n",
                         "src/engine/net/Cargo.toml": b"[package]\nname = 'x'\n"})
        self.assertEqual(before, source_identity.digest(self.root))


class UnitTests(unittest.TestCase):
    def test_unit_reports_the_digest(self):
        text = source_identity.unit("ab" * 32)
        self.assertIn('return "' + "ab" * 32 + '";', text)
        self.assertIn("std::string_view sourceIdentity()", text)
        self.assertIn('#include "game/app/SourceIdentity.h"', text)

    def test_write_touches_the_file_only_when_it_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "generated" / "SourceIdentity.cpp"
            self.assertTrue(source_identity.write(path, "one\n"))
            self.assertFalse(source_identity.write(path, "one\n"))
            self.assertTrue(source_identity.write(path, "two\n"))
            self.assertEqual(path.read_text(encoding="utf-8"), "two\n")

    def test_main_prints_the_digest_and_writes_on_request(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            tree(root, {"VERSION": b"0.0.1\n", "src/a.cpp": b"\n"})
            output = root / "SourceIdentity.cpp"
            import contextlib
            import io
            printed = io.StringIO()
            with contextlib.redirect_stdout(printed):
                self.assertEqual(source_identity.main(["--root", str(root), "--write", str(output)]), 0)
            self.assertEqual(printed.getvalue().strip(), source_identity.digest(root))
            self.assertIn(source_identity.digest(root), output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
