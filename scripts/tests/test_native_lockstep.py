import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from io import StringIO
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import native_lockstep
from native_determinism import ReportError


class Child:
    """Process lifecycle only; no networking or simulation is mocked as evidence."""

    def __init__(self, returncode=None, stubborn=False):
        self.returncode = returncode
        self.stubborn = stubborn
        self.terminations = 0
        self.kills = 0
        self.waits = []

    def poll(self):
        return self.returncode

    def terminate(self):
        self.terminations += 1
        if not self.stubborn:
            self.returncode = -15

    def kill(self):
        self.kills += 1
        self.returncode = -9

    def wait(self, timeout=None):
        self.waits.append(timeout)
        if self.returncode is None:
            raise subprocess.TimeoutExpired("nativeprobe", timeout)
        return self.returncode


def report(command, *, delivery="iroh-lockstep"):
    flags = dict(zip(command[1::2], command[2::2]))
    ticks = int(flags["--ticks"])
    metadata = {
        "type": "metadata", "schema": 1, "simulation": "PlayScene::update",
        "stage": flags["--stage"], "ticks": ticks, "players": int(flags["--players"]), "tick_rate": 60,
        "render_every": int(flags["--render-every"]), "workload": flags["--workload"],
        "input_version": 1, "initialization": "native-defaults-v1",
        "input_delivery": delivery,
        "input_delay": int(flags["--input-delay"]) if delivery == "iroh-lockstep" else 0,
        "complete_state": False, "unobserved": ["Uncaptured presentation"],
        "subsystems": ["party"], "assets_digest": "a" * 64,
        "data_digest": "b" * 64, "config_digest": "c" * 64,
    }
    rows = [metadata]
    for tick in range(1, ticks + 1):
        rows.append({"type": "tick", "tick": tick, "input_digest": f"{tick:064x}",
                     "subsystems": {"party": "d" * 64}, "unavailable": {}})
    rows.append({"type": "complete", "ticks": ticks, "last_tick": ticks})
    Path(flags["--report"]).write_text(
        "".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")


class NativeLockstepRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="native-lockstep-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.probe = self.directory / "nativeprobe.exe"
        self.probe.touch()
        self.children = []
        self.streams = []

    def run_pair(self, **changes):
        options = dict(ticks=3, stage="G1", workload="combat", render_every=[0, 1],
                       delay=3, timeout=7)
        options.update(changes)
        return native_lockstep.run_pair(self.probe, self.directory, **options)

    def start(self, command, kwargs, child, *, ticket=True, emit=True, delivery="iroh-lockstep"):
        self.assertEqual(command[0], str(self.probe))
        self.assertIs(kwargs["stdin"], subprocess.DEVNULL)
        self.assertEqual(kwargs["stderr"], subprocess.STDOUT)
        self.assertFalse(kwargs.get("shell", False))
        self.streams.append(kwargs["stdout"])
        self.children.append(child)
        flags = dict(zip(command[1::2], command[2::2]))
        self.assertIn(int(flags["--players"]), (2, 3, 4))
        self.assertEqual(int(flags["--network-peer"]), len(self.children))
        if flags["--network-role"] == "host" and ticket:
            Path(flags["--ticket"]).write_text("private-test-ticket", encoding="ascii")
        if emit:
            report(command, delivery=delivery)
        return child

    def assert_reaped(self):
        for child in self.children:
            self.assertIsNotNone(child.returncode)
            self.assertTrue(child.waits)
        self.assertTrue(all(stream.closed for stream in self.streams))

    @patch("native_lockstep.subprocess.Popen")
    def test_success_uses_two_roles_and_verifies_real_network_report_contract(self, popen):
        popen.side_effect = lambda command, **kwargs: self.start(command, kwargs, Child(0))
        result = self.run_pair(assets=self.directory / "native assets", data=self.directory / "data")
        self.assertEqual(result["status"], "observed-equal")
        self.assertIs(result["complete_state"], False)
        self.assertEqual(result["metadata"]["input_delivery"], "iroh-lockstep")
        self.assertEqual(result["metadata"]["input_delay"], 3)
        self.assertEqual(result["render_every"], [0, 1])
        commands = [call.args[0] for call in popen.call_args_list]
        self.assertEqual(len(commands), 2)
        self.assertIn("host", commands[0])
        self.assertIn("guest", commands[1])
        self.assertTrue((self.directory / "private-ticket.txt").is_file())
        self.assert_reaped()

    @patch("native_lockstep.subprocess.Popen")
    def test_three_and_four_peers_each_own_one_seat_and_every_report_is_compared(self, popen):
        for peers in (3, 4):
            with self.subTest(peers=peers):
                output = self.directory / f"peers-{peers}"
                output.mkdir()
                self.children.clear()
                self.streams.clear()
                popen.reset_mock()
                popen.side_effect = lambda command, **kwargs: self.start(command, kwargs, Child(0))
                result = native_lockstep.run_pair(
                    self.probe, output, ticks=3, stage="G1", workload="combat",
                    render_every=list(range(peers)), delay=3, timeout=7, peers=peers)
                self.assertEqual(result["status"], "observed-equal")
                self.assertFalse(result["complete_state"])
                self.assertEqual(result["peers"], peers)
                self.assertEqual(result["metadata"]["players"], peers)
                self.assertEqual(result["render_every"], list(range(peers)))
                self.assertEqual(len(result["reports"]), peers)
                commands = [dict(zip(call.args[0][1::2], call.args[0][2::2]))
                            for call in popen.call_args_list]
                self.assertEqual([int(command["--network-peer"]) for command in commands],
                                 list(range(1, peers + 1)))
                self.assertEqual([command["--network-role"] for command in commands],
                                 ["host"] + ["guest"] * (peers - 1))
                self.assertEqual(len({command["--ticket"] for command in commands}), 1)
                self.assert_reaped()

    @patch("native_lockstep.subprocess.Popen")
    def test_fourth_report_divergence_is_not_hidden_by_first_pair_equality(self, popen):
        def launch(command, **kwargs):
            child = self.start(command, kwargs, Child(0))
            flags = dict(zip(command[1::2], command[2::2]))
            if flags["--network-peer"] == "4":
                path = Path(flags["--report"])
                rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
                rows[2]["subsystems"]["party"] = "e" * 64
                path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
            return child
        popen.side_effect = launch
        with self.assertRaisesRegex(ReportError, "Peer 4.*tick 2.*party"):
            self.run_pair(peers=4, render_every=[0, 1, 2, 3])
        self.assert_reaped()

    @patch("native_lockstep.subprocess.Popen")
    def test_fourth_peer_launch_error_reaps_all_three_started_processes(self, popen):
        def launch(command, **kwargs):
            if len(self.children) == 3:
                raise OSError("fourth peer launch failed")
            return self.start(command, kwargs, Child(), emit=False)
        popen.side_effect = launch
        with self.assertRaisesRegex(OSError, "fourth peer launch"):
            self.run_pair(peers=4, render_every=[0, 1, 2, 3])
        self.assertEqual(len(self.children), 3)
        self.assertTrue(all(child.terminations == 1 for child in self.children))
        self.assert_reaped()

    @patch("native_lockstep.subprocess.Popen")
    def test_last_peer_missing_or_truncated_report_refuses_all_peer_equality(self, popen):
        for truncated in (False, True):
            with self.subTest(truncated=truncated):
                output = self.directory / f"missing-{truncated}"
                output.mkdir()
                self.children.clear()
                def launch(command, **kwargs):
                    flags = dict(zip(command[1::2], command[2::2]))
                    last = flags["--network-peer"] == "3"
                    child = self.start(command, kwargs, Child(0), emit=not last or truncated)
                    if last and truncated:
                        path = Path(flags["--report"])
                        path.write_bytes(path.read_bytes()[:-20])
                    return child
                popen.side_effect = launch
                with self.assertRaisesRegex(ReportError, "Peer 3"):
                    native_lockstep.run_pair(
                        self.probe, output, ticks=3, stage="G1", workload="combat",
                        render_every=[0, 1, 2], delay=3, timeout=7, peers=3)
                self.assert_reaped()

    @patch("native_lockstep.subprocess.Popen")
    def test_host_failure_before_ticket_does_not_launch_guest(self, popen):
        popen.side_effect = lambda command, **kwargs: self.start(
            command, kwargs, Child(4), ticket=False, emit=False)
        with self.assertRaisesRegex(ReportError, "before invitation"):
            self.run_pair()
        self.assertEqual(popen.call_count, 1)
        self.assertEqual(self.children[0].terminations, 0)
        self.assert_reaped()

    @patch("native_lockstep.subprocess.Popen")
    def test_guest_launch_error_terminates_and_reaps_already_started_host(self, popen):
        def launch(command, **kwargs):
            if self.children:
                raise OSError("guest could not start")
            return self.start(command, kwargs, Child(), emit=False)
        popen.side_effect = launch
        with self.assertRaisesRegex(OSError, "guest could not start"):
            self.run_pair()
        self.assertEqual(self.children[0].terminations, 1)
        self.assert_reaped()

    @patch("native_lockstep.subprocess.Popen")
    def test_failed_peer_cannot_pass_complete_reports_and_reaps_running_other_peer(self, popen):
        popen.side_effect = lambda command, **kwargs: self.start(
            command, kwargs, Child(7 if self.children else None))
        with self.assertRaisesRegex(ReportError, "process failed"):
            self.run_pair()
        self.assertEqual(self.children[0].terminations, 1)
        self.assertEqual(self.children[1].terminations, 0)
        self.assert_reaped()

    @patch("native_lockstep.time.sleep")
    @patch("native_lockstep.time.monotonic", side_effect=[0, 100])
    @patch("native_lockstep.subprocess.Popen")
    def test_timeout_escalates_unresponsive_children_and_reaps_both(self, popen, _clock, _sleep):
        popen.side_effect = lambda command, **kwargs: self.start(
            command, kwargs, Child(stubborn=True), emit=False)
        with self.assertRaisesRegex(ReportError, "run timed out"):
            self.run_pair()
        for child in self.children:
            self.assertEqual(child.terminations, 1)
            self.assertEqual(child.kills, 1)
            self.assertEqual(child.waits, [5, None])
        self.assert_reaped()

    @patch("native_lockstep.time.sleep", side_effect=KeyboardInterrupt)
    @patch("native_lockstep.time.monotonic", return_value=0)
    @patch("native_lockstep.subprocess.Popen")
    def test_interrupt_after_starting_both_peers_reaps_children(self, popen, _clock, _sleep):
        popen.side_effect = lambda command, **kwargs: self.start(command, kwargs, Child(), emit=False)
        with self.assertRaises(KeyboardInterrupt):
            self.run_pair()
        self.assertEqual(len(self.children), 2)
        self.assert_reaped()

    @patch("native_lockstep.subprocess.Popen")
    def test_success_exit_without_reports_is_not_evidence(self, popen):
        popen.side_effect = lambda command, **kwargs: self.start(command, kwargs, Child(0), emit=False)
        with self.assertRaises((OSError, ReportError)):
            self.run_pair()
        self.assert_reaped()

    @patch("native_lockstep.subprocess.Popen")
    def test_local_mode_reports_cannot_satisfy_network_run(self, popen):
        popen.side_effect = lambda command, **kwargs: self.start(
            command, kwargs, Child(0), delivery="local")
        with self.assertRaisesRegex(ReportError, "Requested input_delivery"):
            self.run_pair()
        self.assert_reaped()

    @patch("native_lockstep.subprocess.Popen")
    def test_existing_ticket_reports_or_logs_are_never_reused(self, popen):
        for name in ("private-ticket.txt", "run-1.jsonl", "run-2.jsonl", "run-1.log", "run-2.log"):
            with self.subTest(name=name):
                path = self.directory / name
                path.touch()
                with self.assertRaisesRegex(ReportError, "existing"):
                    self.run_pair()
                path.unlink()
        popen.assert_not_called()

    @patch("native_lockstep.subprocess.Popen")
    def test_invalid_cli_bounds_never_launch_processes(self, popen):
        for arguments in (("--delay", "-1"), ("--delay", "7"), ("--ticks", "0"),
                          ("--ticks", "36001"), ("--timeout", "nan"), ("--timeout", "inf"),
                          ("--timeout", "0"), ("--render-every", "0", "61")):
            with self.subTest(arguments=arguments), redirect_stderr(StringIO()):
                with self.assertRaises(SystemExit) as error:
                    native_lockstep.main(list(arguments))
                self.assertEqual(error.exception.code, 2)
        popen.assert_not_called()

    @patch("native_lockstep.subprocess.Popen")
    def test_invalid_peer_counts_and_cadence_arity_never_launch(self, popen):
        for peers, cadences in ((1, [0]), (5, [0] * 5), (3, [0, 1]), (4, [0, 1, 2]),
                                (2, [0, 1, 2]), (True, [0, 1])):
            with self.subTest(peers=peers, cadences=cadences):
                with self.assertRaises(ReportError):
                    self.run_pair(peers=peers, render_every=cadences)
        for arguments in (("--peers", "1"), ("--peers", "5"),
                          ("--peers", "3", "--render-every", "0", "1"),
                          ("--peers", "4", "--render-every", "0", "1", "2", "61")):
            with self.subTest(arguments=arguments), redirect_stderr(StringIO()):
                with self.assertRaises(SystemExit) as error:
                    native_lockstep.main(list(arguments))
                self.assertEqual(error.exception.code, 2)
        popen.assert_not_called()


if __name__ == "__main__":
    unittest.main()
