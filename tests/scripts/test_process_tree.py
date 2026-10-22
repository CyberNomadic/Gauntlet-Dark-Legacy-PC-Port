"""A helper notices the process it answers to is gone, and ends its own tree."""
import os
import pathlib
import subprocess
import sys
import threading
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import process_tree  # noqa: E402

SLEEPER = [sys.executable, "-c", "import time; time.sleep(60)"]


def listed(pid, expected, seconds=5):
    """Whether the process table lists `pid`, polled until it matches `expected`."""
    deadline = time.monotonic() + seconds
    while True:
        present = pid in process_tree.table()
        if present == expected or time.monotonic() >= deadline:
            return present
        time.sleep(0.1)


class AncestorTests(unittest.TestCase):
    TABLE = {1: (0, "init"), 10: (1, "Git.exe"), 20: (10, "sh"), 30: (20, "cmd.exe"),
             40: (30, "python")}

    def test_the_nearest_named_ancestor_is_found_through_wrappers(self):
        self.assertEqual(process_tree.nearest_ancestor(self.TABLE, 40, process_tree.GIT), 10)

    def test_a_process_without_such_an_ancestor_has_none(self):
        self.assertIsNone(process_tree.nearest_ancestor(self.TABLE, 10, process_tree.GIT))
        self.assertIsNone(process_tree.nearest_ancestor(self.TABLE, 99, process_tree.GIT))

    def test_a_reused_parent_id_cannot_loop_forever(self):
        self.assertIsNone(process_tree.nearest_ancestor({5: (6, "a"), 6: (5, "b")}, 5, ("git",)))

    def test_the_live_table_lists_this_process_under_its_parent(self):
        table = process_tree.table()
        self.assertIn(os.getpid(), table)
        parent, name = table[os.getpid()]
        self.assertEqual(parent, os.getppid())
        self.assertIn("python", name.lower())


class ExitTests(unittest.TestCase):
    def test_the_callback_runs_once_the_process_exits(self):
        child = subprocess.Popen(SLEEPER)
        gone = threading.Event()
        process_tree.stop_when_gone(child.pid, gone.set)
        self.assertFalse(gone.wait(0.5))
        child.kill()
        child.wait()
        self.assertTrue(gone.wait(5))

    def test_an_exited_process_is_already_gone(self):
        child = subprocess.Popen([sys.executable, "-c", "pass"])
        child.wait()
        started = time.monotonic()
        self.assertTrue(process_tree.wait_for_exit(child.pid))
        self.assertLess(time.monotonic() - started, 5)


class TreeTests(unittest.TestCase):
    def test_stop_ends_the_child_and_the_process_under_it(self):
        script = ("import subprocess, sys, time\n"
                  "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
                  "print(child.pid, flush=True)\n"
                  "time.sleep(60)\n")
        process = process_tree.start([sys.executable, "-c", script], stdout=subprocess.PIPE,
                                     text=True)
        grandchild = int(process.stdout.readline())
        self.assertTrue(listed(grandchild, True))
        process_tree.stop(process)
        self.assertIsNotNone(process.poll())
        self.assertFalse(listed(grandchild, False))

    def test_stopping_an_exited_child_is_harmless(self):
        process = process_tree.start([sys.executable, "-c", "pass"])
        process.wait()
        process_tree.stop(process)
        self.assertEqual(process.returncode, 0)


if __name__ == "__main__":
    unittest.main()
