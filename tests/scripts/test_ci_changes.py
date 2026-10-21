"""CI skips its build, test and lint jobs only when a change is documentation alone."""
import contextlib
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import ci_changes


class ClassificationTests(unittest.TestCase):
    def test_documents_never_count_as_code(self):
        for path in ("README.md", "docs/changelog/1.3.4-alpha.3.md", "docs/notes.txt", "LICENSE",
                     "LICENSE-THIRD-PARTY", ".gitignore", "CONTRIBUTING.md"):
            with self.subTest(path=path):
                self.assertTrue(ci_changes.is_document(path))

    def test_anything_the_build_tests_or_lint_read_counts_as_code(self):
        for path in ("src/game/app/Gauntlet.cpp", "tests/game/app/GauntletTests.cpp", "VERSION",
                     "CMakeLists.txt", "vcpkg.json", ".clang-tidy", ".github/workflows/ci.yml",
                     "scripts/lint.py", "shaders/immediate.frag", "data/text/en.json",
                     "assets/README.md", "src/engine/README.md", "scripts/hooks/pre-push"):
            with self.subTest(path=path):
                self.assertFalse(ci_changes.is_document(path), path)

    def test_a_mixed_change_is_code(self):
        self.assertFalse(ci_changes.touches_code(["README.md", "docs/a.md"]))
        self.assertTrue(ci_changes.touches_code(["README.md", "src/a.cpp"]))
        self.assertFalse(ci_changes.touches_code([]))


class GitTests(unittest.TestCase):
    def test_paths_come_from_the_range_and_an_unknown_base_is_none(self):
        completed = subprocess.CompletedProcess([], 0, "README.md\nsrc/a.cpp\n", "")
        with mock.patch.object(ci_changes.subprocess, "run", return_value=completed) as run:
            self.assertEqual(ci_changes.changed_paths("abc", "def"), ["README.md", "src/a.cpp"])
        self.assertIn("abc..def", run.call_args.args[0])
        self.assertIsNone(ci_changes.changed_paths("0" * 40, "def"))
        self.assertIsNone(ci_changes.changed_paths(None, "def"))
        failed = subprocess.CompletedProcess([], 128, "", "unknown revision")
        with mock.patch.object(ci_changes.subprocess, "run", return_value=failed):
            self.assertIsNone(ci_changes.changed_paths("abc", "def"))


class MainTests(unittest.TestCase):
    def run_main(self, paths, output=None):
        arguments = ["--base", "abc", "--head", "def"] + (["--output", str(output)] if output else [])
        with mock.patch.object(ci_changes, "changed_paths", return_value=paths), \
             contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(ci_changes.main(arguments), 0)
        return out.getvalue()

    def test_documents_only_reports_false_and_appends_the_output_line(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "github-output"
            output.write_text("other=1\n", encoding="utf-8")
            printed = self.run_main(["README.md"], output)
            self.assertTrue(printed.startswith("code=false"))
            self.assertEqual(output.read_text(encoding="utf-8"), "other=1\ncode=false\n")

    def test_code_and_unknown_bases_report_true(self):
        self.assertTrue(self.run_main(["src/a.cpp"]).startswith("code=true"))
        self.assertTrue(self.run_main(None).startswith("code=true"))


if __name__ == "__main__":
    unittest.main()
