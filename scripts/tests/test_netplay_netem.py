import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from netplay_netem import commands, main, owns_root, safe_initial


class NetworkImpairmentTests(unittest.TestCase):
    def test_every_command_is_namespace_scoped(self):
        for command in commands("gdl-guest", "eth0", 50, 10, 1):
            self.assertEqual(command[:4], ["ip", "netns", "exec", "gdl-guest"])
            self.assertIn("eth0", command)

    def test_rejects_invalid_names_and_values(self):
        for name in ("", "-a", "x;reboot", "a/b"):
            with self.assertRaises(ValueError):
                commands(name, "eth0", 50, 0, 0)
        for delay, jitter, loss in ((-1, 0, 0), (10, 20, 0), (50, 0, 100), (float("nan"), 0, 0)):
            with self.assertRaises(ValueError):
                commands("guest", "eth0", delay, jitter, loss)

    def test_never_replaces_or_deletes_foreign_root(self):
        self.assertTrue(safe_initial([{"root": True, "kind": "noqueue"}]))
        self.assertFalse(safe_initial([{"root": True, "kind": "fq_codel"}]))
        self.assertFalse(owns_root([{"root": True, "kind": "netem", "handle": "10:"}]))
        self.assertTrue(owns_root([{"root": True, "kind": "netem", "handle": "7d1:"}]))

    @patch("netplay_netem.sys.platform", "linux")
    @patch("netplay_netem.time.sleep", side_effect=KeyboardInterrupt)
    @patch("netplay_netem.subprocess.run")
    def test_interrupt_removes_only_owned_queue(self, run, sleep):
        run.side_effect = [SimpleNamespace(stdout='[{"root":true,"kind":"noqueue"}]'),
                           SimpleNamespace(),
                           SimpleNamespace(stdout='[{"root":true,"kind":"netem","handle":"7d1:"}]'),
                           SimpleNamespace()]
        self.assertEqual(main(["--namespace", "guest", "--interface", "eth0", "--apply"]), 130)
        self.assertIn("del", run.call_args_list[-1].args[0])
        self.assertEqual(run.call_args_list[-1].args[0][:4], ["ip", "netns", "exec", "guest"])

    @patch("netplay_netem.subprocess.run")
    def test_preview_never_calls_network_tools(self, run):
        self.assertEqual(main(["--namespace", "guest", "--interface", "eth0"]), 0)
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
