import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from io import StringIO
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "netplay"))
import native_rollback
from native_determinism import (IMPAIRMENT_COUNTS, IMPAIRMENT_DEFAULTS, IMPAIRMENT_TIMINGS, OUTAGE_PROGRESS_COUNTS,
                                ReportError, compare_reports)


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


def report(command, *, delivery="iroh-rollback"):
    flags = dict(zip(command[1::2], command[2::2]))
    ticks = int(flags["--ticks"])
    metadata = {
        "type": "metadata", "schema": 2, "simulation": "NativeRollbackDriver",
        "stage": flags["--stage"], "ticks": ticks, "players": int(flags["--players"]), "tick_rate": 60,
        "render_every": int(flags["--render-every"]), "workload": flags["--workload"],
        "input_version": 1, "initialization": "native-defaults-v1",
        "agreement_schema": "native-scene-and-control-v1", "runtime_digest": "e" * 64,
        "input_delivery": delivery,
        "input_delay": int(flags["--input-delay"]) if delivery == "iroh-rollback" else 0,
        "complete_state": False, "unobserved": ["Uncaptured presentation"],
        "subsystems": ["party"], "assets_digest": "a" * 64,
        "data_digest": "b" * 64, "config_digest": "c" * 64,
    }
    rows = [metadata]
    for tick in range(1, ticks + 1):
        rows.append({"type": "tick", "tick": tick, "input_digest": f"{tick:064x}",
                     "event_digest": "d" * 64, "audio_digest": "e" * 64})
        if tick % 30 == 0:
            rows.append({"type": "confirmed", "tick": tick, "digest": "f" * 64})
    rows.append({"type": "complete", "ticks": ticks, "last_tick": ticks,
                 "native_digest": "f" * 64, "metrics": {
                     "sampled_inputs": ticks, "native_ticks": ticks, "replay_ticks": 0,
                     "corrections": 0, "max_replay": 0, "stalls": 0, "pacing_idles": 0,
                     "prediction_max_ticks": 0, "input_sample_to_first_native_update_p50_ms": 0.0,
                     "input_sample_to_first_native_update_p95_ms": 1.0,
                     "correction_p50_ms": 0.0, "correction_p95_ms": 0.0,
                     "correction_max_ms": 0.0, "link_rtt_ms": [-1]}})
    if "--network-path" in flags:
        path = flags["--network-path"]
        metadata["network_path"] = path
        local = int(flags["--network-peer"])
        peers = int(flags["--players"])
        links = list(range(2, peers + 1)) if local == 1 else [1]
        # Synthetic path observations validate report/launcher contracts only.
        # They are not proof of public relay access or measured networking.
        rows[-1]["metrics"]["transport"] = {"local_peer": local, "links": [
            {"peer": peer, "samples": 2, "relay_samples": 1 if path == "relay" else 0,
             "direct_samples": 0 if path == "relay" else 1, "unavailable_samples": 1,
             "rtt_p50_ms": 40.0 if path == "relay" else 0.0,
             "rtt_p95_ms": 40.0 if path == "relay" else 0.0,
             "rtt_max_ms": 40.0 if path == "relay" else 0.0} for peer in links]}
        rows[-1]["metrics"]["link_rtt_ms"] = [-1] * len(links)
    if "--input-impairment-seed" in flags:
        # Synthetic report rows test only parser/launcher contracts. They are
        # never networking, delay or recovery evidence for a native run.
        options = {}
        for key in IMPAIRMENT_DEFAULTS:
            flag = "impairment-seed" if key == "seed" else key.replace("_", "-")
            options[key] = int(flags[f"--input-{flag}"])
        profile = native_rollback.input_impairment(options, int(flags["--players"]), ticks)
        metadata["impairment"] = profile
        injected = {key: 0 for key in IMPAIRMENT_COUNTS | IMPAIRMENT_TIMINGS}
        injected.update(attempted=10, transmitted=10, outage_finished=False)
        injected["outage_progress"] = {key: 0 for key in OUTAGE_PROGRESS_COUNTS}
        if profile["mode"] != "none":
            injected.update(enqueued=10, queue_peak=1,
                            residence_p50_ms=float(profile["latency_ms"]),
                            residence_p95_ms=float(profile["latency_ms"]),
                            residence_max_ms=float(profile["latency_ms"]))
        if profile["outage_peer"] == int(flags.get("--network-peer", 0)) and profile["outage_ms"]:
            injected.update(attempted=14, outage_dropped=4, outage_started_tick=profile["outage_tick"],
                            outage_finished=True, outage_elapsed_ms=float(profile["outage_ms"]),
                            oldest_recovery_datagrams=1)
            rows[-1]["metrics"]["stalls"] = 1
            rows[-1]["metrics"]["prediction_max_ticks"] = 2
            tick = profile["outage_tick"]
            injected["outage_progress"].update(
                updates=2, stalls=1, first_next_tick=tick, last_next_tick=tick + 1,
                first_confirmed_tick=tick - 2, last_confirmed_tick=tick - 2,
                prediction_max_ticks=2)
        rows[-1]["metrics"]["impairment"] = injected
    Path(flags["--report"]).write_text(
        "".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")


class NativeRollbackRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="native-rollback-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.probe = self.directory / "nativeprobe.exe"
        self.probe.touch()
        self.children = []
        self.streams = []

    def run_pair(self, **changes):
        options = dict(ticks=3, stage="G1", workload="combat", render_every=[0, 1],
                       delay=1, timeout=7)
        options.update(changes)
        return native_rollback.run_pair(self.probe, self.directory, **options)

    def start(self, command, kwargs, child, *, ticket=True, emit=True, delivery="iroh-rollback"):
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

    @patch("native_rollback.subprocess.Popen")
    def test_success_uses_two_roles_and_verifies_real_network_report_contract(self, popen):
        popen.side_effect = lambda command, **kwargs: self.start(command, kwargs, Child(0))
        result = self.run_pair(assets=self.directory / "native assets", data=self.directory / "data")
        self.assertEqual(result["status"], "observed-equal")
        self.assertIs(result["complete_state"], False)
        self.assertEqual(result["metadata"]["input_delivery"], "iroh-rollback")
        self.assertEqual(result["metadata"]["input_delay"], 1)
        self.assertEqual(result["metadata"]["network_path"], "local")
        self.assertEqual(result["metadata"]["impairment"]["mode"], "none")
        self.assertEqual(result["render_every"], [0, 1])
        commands = [call.args[0] for call in popen.call_args_list]
        self.assertEqual(len(commands), 2)
        self.assertIn("host", commands[0])
        self.assertIn("guest", commands[1])
        self.assertTrue((self.directory / "private-ticket.txt").is_file())
        self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
    def test_relay_is_explicit_and_requires_per_link_provider_evidence(self, popen):
        popen.side_effect = lambda command, **kwargs: self.start(command, kwargs, Child(0))
        result = self.run_pair(network_path="relay", peers=4, render_every=[0, 1, 2, 3])
        self.assertEqual(result["metadata"]["network_path"], "relay")
        self.assertEqual([len(peer["transport"]["links"]) for peer in result["metrics"]], [3, 1, 1, 1])
        for call in popen.call_args_list:
            flags = dict(zip(call.args[0][1::2], call.args[0][2::2]))
            self.assertEqual(flags["--network-path"], "relay")
        self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
    def test_requested_relay_cannot_pass_unmeasured_direct_or_missing_links(self, popen):
        def unknown(rows):
            rows[-1]["metrics"]["transport"]["links"][0].update(
                relay_samples=0, unavailable_samples=2, rtt_p50_ms=0, rtt_p95_ms=0, rtt_max_ms=0)
        edits = (
            unknown,
            lambda rows: rows[-1]["metrics"]["transport"]["links"][0].update(
                relay_samples=0, direct_samples=1),
            lambda rows: rows[-1]["metrics"]["transport"]["links"].pop(),
            lambda rows: rows[-1]["metrics"].pop("transport"),
            lambda rows: rows[0].update(network_path="local"),
            lambda rows: rows[0].pop("network_path"),
            lambda rows: rows[-1]["metrics"]["transport"].update(local_peer=4),
        )
        for index, edit in enumerate(edits):
            with self.subTest(index=index):
                output = self.directory / f"unproven-relay-{index}"
                output.mkdir()
                self.children.clear()
                self.streams.clear()
                def launch(command, **kwargs):
                    child = self.start(command, kwargs, Child(0))
                    flags = dict(zip(command[1::2], command[2::2]))
                    if flags["--network-peer"] == "3":
                        path = Path(flags["--report"])
                        rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
                        edit(rows)
                        path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
                    return child
                popen.side_effect = launch
                with self.assertRaises(ReportError):
                    native_rollback.run_pair(self.probe, output, ticks=3, stage="G1", workload="combat",
                                             render_every=[0, 1, 2, 3], delay=1, timeout=7, peers=4,
                                             network_path="relay")
                self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
    def test_unknown_network_paths_never_launch(self, popen):
        for path in ("automatic", "RELAYS", None, True):
            with self.subTest(path=path), self.assertRaises(ReportError):
                self.run_pair(network_path=path)
        with redirect_stderr(StringIO()), self.assertRaises(SystemExit):
            native_rollback.main(["--network-path", "automatic"])
        popen.assert_not_called()

    @patch("native_rollback.subprocess.Popen")
    def test_three_and_four_peer_impairment_options_require_explicit_recovery_evidence(self, popen):
        profile = {"latency_ms": 40, "jitter_ms": 10, "loss_ppm": 20_000,
                   "reorder_ppm": 100_000, "reorder_ms": 35, "seed": 17,
                   "outage_tick": 12, "outage_ms": 1000, "outage_peer": 2}
        for peers in (3, 4):
            with self.subTest(peers=peers):
                output = self.directory / f"impaired-{peers}"
                output.mkdir()
                self.children.clear()
                self.streams.clear()
                popen.reset_mock()
                popen.side_effect = lambda command, **kwargs: self.start(command, kwargs, Child(0))
                result = native_rollback.run_pair(
                    self.probe, output, ticks=30, stage="G1", workload="combat",
                    render_every=list(range(peers)), delay=1, timeout=7, peers=peers,
                    impairment=profile)
                self.assertEqual(result["metadata"]["impairment"]["mode"], "simulated-application-input")
                self.assertIs(result["metadata"]["impairment"]["reliable_impaired"], False)
                self.assertEqual(result["outage_recovery"]["all_peers_confirmed_through"], 30)
                self.assertEqual(result["outage_recovery"]["oldest_recovery_datagrams"], 1)
                self.assertEqual(result["metrics"][1]["link_rtt_ms"], [-1])
                for call in popen.call_args_list:
                    flags = dict(zip(call.args[0][1::2], call.args[0][2::2]))
                    for key, value in profile.items():
                        flag = "impairment-seed" if key == "seed" else key.replace("_", "-")
                        self.assertEqual(flags[f"--input-{flag}"], str(value))
                self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
    def test_requested_outage_without_oldest_input_recovery_is_not_a_pass(self, popen):
        def launch(command, **kwargs):
            child = self.start(command, kwargs, Child(0))
            flags = dict(zip(command[1::2], command[2::2]))
            if flags["--network-peer"] == "2":
                path = Path(flags["--report"])
                rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
                rows[-1]["metrics"]["impairment"]["oldest_recovery_datagrams"] = 0
                path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
            return child
        popen.side_effect = launch
        with self.assertRaisesRegex(ReportError, "oldest-input recovery"):
            self.run_pair(ticks=30, impairment={"outage_tick": 12, "outage_ms": 1000, "outage_peer": 2})
        self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
    def test_pacing_waits_inside_outage_can_recover_without_hitting_prediction_cap(self, popen):
        def launch(command, **kwargs):
            child = self.start(command, kwargs, Child(0))
            flags = dict(zip(command[1::2], command[2::2]))
            if flags["--network-peer"] == "2":
                path = Path(flags["--report"])
                rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
                rows[-1]["metrics"].update(stalls=0, pacing_idles=1)
                rows[-1]["metrics"]["impairment"]["outage_progress"].update(stalls=0, pacing_idles=1)
                path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
            return child
        popen.side_effect = launch
        result = self.run_pair(ticks=30, delay=2,
                               impairment={"outage_tick": 12, "outage_ms": 1000, "outage_peer": 2})
        self.assertTrue(all(peer["stalls"] == 0 for peer in result["metrics"]))
        progress = result["outage_recovery"]["outage_progress"]
        self.assertEqual(progress["pacing_idles"], 1)
        self.assertGreater(progress["last_next_tick"], progress["first_next_tick"])
        self.assertLess(progress["prediction_max_ticks"], 8)
        self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
    def test_missing_window_or_unrelated_lifetime_waits_do_not_prove_outage_recovery(self, popen):
        for missing in (False, True):
            with self.subTest(missing=missing):
                output = self.directory / f"outage-window-{missing}"
                output.mkdir()
                self.children.clear()
                self.streams.clear()
                def launch(command, **kwargs):
                    child = self.start(command, kwargs, Child(0))
                    flags = dict(zip(command[1::2], command[2::2]))
                    if flags["--network-peer"] == "2":
                        path = Path(flags["--report"])
                        rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
                        metrics = rows[-1]["metrics"]
                        metrics.update(stalls=100, pacing_idles=100)
                        if missing:
                            metrics["impairment"].pop("outage_progress")
                        else:
                            metrics["impairment"]["outage_progress"].update(stalls=0, pacing_idles=0)
                        path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
                    return child
                popen.side_effect = launch
                with self.assertRaisesRegex(ReportError, "outage-window native waits"):
                    native_rollback.run_pair(
                        self.probe, output, ticks=30, stage="G1", workload="combat",
                        render_every=[0, 1], delay=2, timeout=7,
                        impairment={"outage_tick": 12, "outage_ms": 1000, "outage_peer": 2})
                self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
    def test_malformed_outage_window_progress_is_not_recovery_evidence(self, popen):
        for index, change in enumerate((
                {"updates": 0}, {"updates": True}, {"stalls": 3}, {"pacing_idles": 1},
                {"first_next_tick": 11}, {"last_next_tick": 15},
                {"last_confirmed_tick": 13}, {"prediction_max_ticks": 9}, {"unexpected": 1})):
            with self.subTest(change=change):
                output = self.directory / f"outage-invalid-{index}"
                output.mkdir()
                self.children.clear()
                self.streams.clear()
                def launch(command, **kwargs):
                    child = self.start(command, kwargs, Child(0))
                    flags = dict(zip(command[1::2], command[2::2]))
                    if flags["--network-peer"] == "2":
                        path = Path(flags["--report"])
                        rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
                        rows[-1]["metrics"]["impairment"]["outage_progress"].update(change)
                        path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
                    return child
                popen.side_effect = launch
                with self.assertRaises(ReportError):
                    native_rollback.run_pair(
                        self.probe, output, ticks=30, stage="G1", workload="combat",
                        render_every=[0, 1], delay=2, timeout=7,
                        impairment={"outage_tick": 12, "outage_ms": 1000, "outage_peer": 2})
                self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
    def test_invalid_impairment_bounds_or_unpaired_settings_never_launch(self, popen):
        invalid = ({"latency_ms": 501}, {"jitter_ms": -1}, {"loss_ppm": 250001},
                   {"loss_ppm": True}, {"reorder_ppm": 1}, {"reorder_ms": 1},
                   {"seed": 0}, {"seed": 2**32}, {"outage_ms": 5001},
                   {"outage_tick": 1}, {"outage_peer": 1},
                   {"outage_tick": 22, "outage_ms": 1000, "outage_peer": 2},
                   {"outage_tick": 12, "outage_ms": 1000, "outage_peer": 3}, {"unexpected": 1},
                   {"mode": "real-internet"}, {"reliable_impaired": True})
        for profile in invalid:
            with self.subTest(profile=profile):
                with self.assertRaises(ReportError):
                    self.run_pair(ticks=30, impairment=profile)
        for args in (("--input-latency-ms", "501"), ("--input-impairment-seed", "0"),
                     ("--input-outage-ms", "1000"), ("--input-reorder-ppm", "5")):
            with self.subTest(args=args), redirect_stderr(StringIO()):
                with self.assertRaises(SystemExit) as error:
                    native_rollback.main(list(args))
                self.assertEqual(error.exception.code, 2)
        popen.assert_not_called()

    @patch("native_rollback.subprocess.Popen")
    def test_three_and_four_peers_each_own_one_seat_and_every_report_is_compared(self, popen):
        for peers in (3, 4):
            with self.subTest(peers=peers):
                output = self.directory / f"peers-{peers}"
                output.mkdir()
                self.children.clear()
                self.streams.clear()
                popen.reset_mock()
                popen.side_effect = lambda command, **kwargs: self.start(command, kwargs, Child(0))
                result = native_rollback.run_pair(
                    self.probe, output, ticks=3, stage="G1", workload="combat",
                    render_every=list(range(peers)), delay=1, timeout=7, peers=peers)
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

    @patch("native_rollback.subprocess.Popen")
    def test_fourth_report_divergence_is_not_hidden_by_first_pair_equality(self, popen):
        def launch(command, **kwargs):
            child = self.start(command, kwargs, Child(0))
            flags = dict(zip(command[1::2], command[2::2]))
            if flags["--network-peer"] == "4":
                path = Path(flags["--report"])
                rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
                rows[2]["event_digest"] = "e" * 64
                path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
            return child
        popen.side_effect = launch
        with self.assertRaisesRegex(ReportError, "Peer 4.*tick 2.*event_digest"):
            self.run_pair(peers=4, render_every=[0, 1, 2, 3])
        self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
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

    @patch("native_rollback.subprocess.Popen")
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
                    native_rollback.run_pair(
                        self.probe, output, ticks=3, stage="G1", workload="combat",
                        render_every=[0, 1, 2], delay=1, timeout=7, peers=3)
                self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
    def test_host_failure_before_ticket_does_not_launch_guest(self, popen):
        popen.side_effect = lambda command, **kwargs: self.start(
            command, kwargs, Child(4), ticket=False, emit=False)
        with self.assertRaisesRegex(ReportError, "before invitation"):
            self.run_pair()
        self.assertEqual(popen.call_count, 1)
        self.assertEqual(self.children[0].terminations, 0)
        self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
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

    @patch("native_rollback.subprocess.Popen")
    def test_failed_peer_cannot_pass_complete_reports_and_reaps_running_other_peer(self, popen):
        popen.side_effect = lambda command, **kwargs: self.start(
            command, kwargs, Child(7 if self.children else None))
        with self.assertRaisesRegex(ReportError, "process failed"):
            self.run_pair()
        self.assertEqual(self.children[0].terminations, 1)
        self.assertEqual(self.children[1].terminations, 0)
        self.assert_reaped()

    @patch("native_rollback.time.sleep")
    @patch("native_rollback.time.monotonic", side_effect=[0, 100])
    @patch("native_rollback.subprocess.Popen")
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

    @patch("native_rollback.time.sleep", side_effect=KeyboardInterrupt)
    @patch("native_rollback.time.monotonic", return_value=0)
    @patch("native_rollback.subprocess.Popen")
    def test_interrupt_after_starting_both_peers_reaps_children(self, popen, _clock, _sleep):
        popen.side_effect = lambda command, **kwargs: self.start(command, kwargs, Child(), emit=False)
        with self.assertRaises(KeyboardInterrupt):
            self.run_pair()
        self.assertEqual(len(self.children), 2)
        self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
    def test_success_exit_without_reports_is_not_evidence(self, popen):
        popen.side_effect = lambda command, **kwargs: self.start(command, kwargs, Child(0), emit=False)
        with self.assertRaises((OSError, ReportError)):
            self.run_pair()
        self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
    def test_local_mode_reports_cannot_satisfy_network_run(self, popen):
        popen.side_effect = lambda command, **kwargs: self.start(
            command, kwargs, Child(0), delivery="local")
        with self.assertRaisesRegex(ReportError, "Unsupported simulation"):
            self.run_pair()
        self.assert_reaped()

    @patch("native_rollback.subprocess.Popen")
    def test_existing_ticket_reports_or_logs_are_never_reused(self, popen):
        for name in ("private-ticket.txt", "run-1.jsonl", "run-2.jsonl", "run-1.log", "run-2.log"):
            with self.subTest(name=name):
                path = self.directory / name
                path.touch()
                with self.assertRaisesRegex(ReportError, "existing"):
                    self.run_pair()
                path.unlink()
        popen.assert_not_called()

    @patch("native_rollback.subprocess.Popen")
    def test_invalid_cli_bounds_never_launch_processes(self, popen):
        for arguments in (("--delay", "-1"), ("--delay", "3"), ("--delay", "7"), ("--ticks", "0"),
                          ("--ticks", "36001"), ("--timeout", "nan"), ("--timeout", "inf"),
                          ("--timeout", "0"), ("--render-every", "0", "61")):
            with self.subTest(arguments=arguments), redirect_stderr(StringIO()):
                with self.assertRaises(SystemExit) as error:
                    native_rollback.main(list(arguments))
                self.assertEqual(error.exception.code, 2)
        popen.assert_not_called()

    @patch("native_rollback.subprocess.Popen")
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
                    native_rollback.main(list(arguments))
                self.assertEqual(error.exception.code, 2)
        popen.assert_not_called()


class NativeRollbackReportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="native-rollback-report-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.left = self.directory / "first.jsonl"
        self.right = self.directory / "second.jsonl"
        for path, cadence in ((self.left, 0), (self.right, 3)):
            report(["probe", "--report", str(path), "--ticks", "60", "--stage", "G2",
                    "--players", "4", "--render-every", str(cadence),
                    "--workload", "combat", "--input-delay", "1"])
        self.original = self.right.read_bytes()

    def mutate(self, change):
        rows = [json.loads(line) for line in self.original.splitlines()]
        change(rows)
        self.right.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")

    def impairment_reports(self):
        for path, cadence in ((self.left, 0), (self.right, 3)):
            command = ["probe", "--report", str(path), "--ticks", "60", "--stage", "G2",
                       "--players", "4", "--render-every", str(cadence),
                       "--workload", "combat", "--input-delay", "1"]
            values = {**IMPAIRMENT_DEFAULTS, "latency_ms": 30, "jitter_ms": 10}
            for key, value in values.items():
                flag = "impairment-seed" if key == "seed" else key.replace("_", "-")
                command.extend([f"--input-{flag}", str(value)])
            report(command)
        self.original = self.right.read_bytes()

    def test_impairment_profiles_are_compared_but_real_rtt_is_not_synthetic(self):
        self.impairment_reports()
        self.mutate(lambda rows: rows[-1]["metrics"].update(link_rtt_ms=[2]))
        result = compare_reports(self.left, self.right)
        self.assertEqual(result["metadata"]["impairment"]["latency_ms"], 30)
        self.assertEqual(result["metrics"][1]["link_rtt_ms"], [2])
        self.mutate(lambda rows: rows[0]["impairment"].update(latency_ms=31))
        with self.assertRaisesRegex(ReportError, "Metadata differs: impairment"):
            compare_reports(self.left, self.right)

    def test_identical_invalid_impairment_profiles_or_accounting_fail_closed(self):
        self.impairment_reports()
        changes = (
            lambda rows: rows[0]["impairment"].update(reliable_impaired=True),
            lambda rows: rows[0]["impairment"].update(mode="real-internet"),
            lambda rows: rows[0]["impairment"].update(scope="reliable-and-input"),
            lambda rows: rows[0]["impairment"].update(seed=True),
            lambda rows: rows[0]["impairment"].update(unexpected=0),
            lambda rows: rows[-1]["metrics"].pop("impairment"),
            lambda rows: rows[-1]["metrics"]["impairment"].update(enqueued=11),
            lambda rows: rows[-1]["metrics"]["impairment"].update(attempted=9),
            lambda rows: rows[-1]["metrics"]["impairment"].update(queue_peak=4097),
            lambda rows: rows[-1]["metrics"]["impairment"].update(reordered=11),
            lambda rows: rows[-1]["metrics"]["impairment"].update(outage_finished=True),
            lambda rows: rows[-1]["metrics"]["impairment"].update(oldest_recovery_datagrams=1),
            lambda rows: rows[-1]["metrics"]["impairment"].update(residence_p50_ms=40),
            lambda rows: rows[-1]["metrics"]["impairment"].update(residence_max_ms=float("inf")),
        )
        for change in changes:
            self.mutate(change)
            self.left.write_bytes(self.right.read_bytes())
            with self.assertRaises(ReportError):
                compare_reports(self.left, self.right)

    def test_impairment_metrics_are_required_only_with_the_matching_metadata_extension(self):
        self.impairment_reports()
        self.mutate(lambda rows: rows[0].pop("impairment"))
        self.left.write_bytes(self.right.read_bytes())
        with self.assertRaises(ReportError):
            compare_reports(self.left, self.right)

    def test_metrics_are_local_measurements_not_consensus(self):
        self.mutate(lambda rows: rows[-1]["metrics"].update(
            input_sample_to_first_native_update_p95_ms=22.5, stalls=13, link_rtt_ms=[41]))
        result = compare_reports(self.left, self.right)
        self.assertEqual(result["status"], "observed-equal")
        self.assertEqual(result["metrics"][1]["stalls"], 13)
        self.assertEqual(result["metrics"][1]["link_rtt_ms"], [41])

    def test_each_confirmed_stream_and_final_native_digest_are_compared(self):
        for field in ("input_digest", "event_digest", "audio_digest"):
            with self.subTest(field=field):
                self.mutate(lambda rows: rows[13].update({field: "0" * 64}))
                with self.assertRaisesRegex(ReportError, f"tick 13, {field}"):
                    compare_reports(self.left, self.right)
        self.mutate(lambda rows: rows[31].update(digest="0" * 64))
        with self.assertRaisesRegex(ReportError, "tick 30, native agreement"):
            compare_reports(self.left, self.right)
        self.mutate(lambda rows: rows[-1].update(native_digest="0" * 64))
        with self.assertRaisesRegex(ReportError, "Final native agreement"):
            compare_reports(self.left, self.right)

    def test_missing_duplicate_out_of_order_and_unconfirmed_rows_are_refused(self):
        edits = (
            lambda rows: rows.pop(31),
            lambda rows: rows.insert(31, rows[31]),
            lambda rows: rows[31].update(tick=60),
            lambda rows: rows[12].update(tick=13),
            lambda rows: rows[-1].update(last_tick=59),
        )
        for edit in edits:
            self.mutate(edit)
            with self.assertRaises(ReportError):
                compare_reports(self.left, self.right)

    def test_same_invalid_metrics_do_not_certify_two_reports(self):
        mutations = (
            {"max_replay": 9}, {"prediction_max_ticks": 9}, {"native_ticks": 59},
            {"corrections": 1}, {"sampled_inputs": True}, {"stalls": -1},
            {"input_sample_to_first_native_update_p50_ms": 9.0},
            {"input_sample_to_first_native_update_p95_ms": float("nan")},
            {"correction_p95_ms": 0.5}, {"link_rtt_ms": []}, {"link_rtt_ms": [True]},
            {"link_rtt_ms": [-2]}, {"link_rtt_ms": [0, 0, 0, 0]},
        )
        for values in mutations:
            with self.subTest(values=values):
                self.mutate(lambda rows: rows[-1]["metrics"].update(values))
                self.left.write_bytes(self.right.read_bytes())
                with self.assertRaises(ReportError):
                    compare_reports(self.left, self.right)

    def test_runtime_contract_and_schema_are_required(self):
        for key in ("runtime_digest", "agreement_schema"):
            self.mutate(lambda rows: rows[0].pop(key))
            with self.assertRaises(ReportError):
                compare_reports(self.left, self.right)
        self.mutate(lambda rows: rows[0].update(runtime_digest="0" * 64))
        with self.assertRaisesRegex(ReportError, "runtime_digest"):
            compare_reports(self.left, self.right)


if __name__ == "__main__":
    unittest.main()
