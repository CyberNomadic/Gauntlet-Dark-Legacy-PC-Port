"""The pre-push hook lints what is pushed and dies with the git that ran it."""
import importlib.machinery
import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
HOOK = ROOT / "scripts" / "hooks" / "pre-push"


def load_hook():
    loader = importlib.machinery.SourceFileLoader("pre_push", str(HOOK))
    spec = importlib.util.spec_from_loader("pre_push", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class RangeTests(unittest.TestCase):
    def test_updated_and_new_refs_are_linted_and_deletions_skipped(self):
        hook = load_hook()
        zero = "0" * 40
        lines = ["refs/heads/main aaa refs/heads/main bbb",
                 "refs/heads/new ccc refs/heads/new " + zero,
                 f"(delete) {zero} refs/heads/old ddd",
                 "malformed line"]
        self.assertEqual(hook.pushed_ranges(lines), [("bbb", "aaa"), (None, "ccc")])


class LintLifetimeTests(unittest.TestCase):
    def test_the_lint_ends_once_the_watched_git_is_gone(self):
        hook = load_hook()
        git = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        self.addCleanup(git.wait)
        self.addCleanup(git.kill)
        with tempfile.TemporaryDirectory() as directory:
            sleeper = pathlib.Path(directory) / "lint.py"
            sleeper.write_text("import time\ntime.sleep(60)\n", encoding="utf-8")
            outcome = {}
            with mock.patch.object(hook, "LINT", sleeper):
                thread = threading.Thread(
                    target=lambda: outcome.update(code=hook.run_lint([], git.pid)))
                thread.start()
                time.sleep(0.5)
                self.assertTrue(thread.is_alive(), "the lint should still be running")
                git.kill()
                git.wait()
                thread.join(timeout=15)
            self.assertFalse(thread.is_alive(), "the lint should have been stopped")
            self.assertNotEqual(outcome["code"], 0)

    def test_a_lint_that_finishes_reports_its_own_exit_code(self):
        hook = load_hook()
        with tempfile.TemporaryDirectory() as directory:
            script = pathlib.Path(directory) / "lint.py"
            script.write_text("import sys\nsys.exit(3)\n", encoding="utf-8")
            with mock.patch.object(hook, "LINT", script):
                self.assertEqual(hook.run_lint([]), 3)


if __name__ == "__main__":
    unittest.main()
