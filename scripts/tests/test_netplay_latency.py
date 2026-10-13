import sys
import unittest
import tempfile
from copy import deepcopy
from contextlib import ExitStack, redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from netplay_latency import attack_response, evaluate, evaluate_stage, launch, main, throw_diagnostics


class LatencyReportTests(unittest.TestCase):
    def test_throw_diagnostics_are_absent_without_observations(self):
        self.assertIsNone(throw_diagnostics(self.report()))

    def test_throw_diagnostics_keep_native_selection_separate_from_projectiles(self):
        report = self.report()
        report["expired_throw_authority"] = 1
        failures = evaluate(report, 75)
        seed = {"tick": 90, "ranged": False, "weapon": False, "melee": True}
        action = {"tick": 102, "action": 53, "action_family": "melee", "generation": 8,
                  "actual_input_tick": 101, "received_input_tick": 104, "reaction": 0,
                  "late_inputs": 0, "ranged": False, "weapon": False}
        origin = {"tick": 105, "generation": 10, "live": True, "ordinal": 0}
        report["throw_diagnostics"] = {
            "scope": "observations, not acknowledgements", "window_ms": 1000,
            "omitted_requests": 2,
            "requests": [{"tick": 100, "seat": 1, "shown_seed": seed, "host_seed": seed,
                          "closed": True, "interrupted": False,
                          "host_observations": [action], "projectile_origins": [origin],
                          "omitted_observations": 3, "omitted_origins": 4}]}
        result = throw_diagnostics(report)
        self.assertEqual(result["omitted_requests"], 2)
        request = result["requests"][0]
        self.assertEqual(request["host_seed"], seed)
        self.assertEqual(request["host_actions"], [action])
        self.assertEqual(request["projectile_origins"], [origin])
        self.assertEqual(request["omitted_observations"], 3)
        self.assertEqual(request["omitted_origins"], 4)
        self.assertEqual(evaluate(report, 75), failures)
        self.assertTrue(failures)

    def test_throw_diagnostics_do_not_label_an_unobserved_request_rejected(self):
        report = {"throw_diagnostics": {"requests": [
            {"tick": 100, "seat": 1, "closed": True, "interrupted": False,
             "host_observations": [], "projectile_origins": []}]}}
        request = throw_diagnostics(report)["requests"][0]
        self.assertEqual(request["host_actions"], [])
        self.assertEqual(request["projectile_origins"], [])
        self.assertNotIn("rejected", request)

    def test_mixed_combat_measurements_cannot_replace_strict_throw_evidence(self):
        report = self.report()
        report["missed_throws"] = 5
        report["expired_throw_authority"] = 5
        failures = evaluate(report, 75)
        self.assertTrue(failures)
        self.assertIsNone(attack_response(report))
        report.update(attack_probe_schema=1,
                      attack_observation_scope="observations, not input or hit ACKs",
                      attack_to_first_frame={"count": 11, "p95_ms": 20},
                      attack_to_host_action={"count": 11, "p95_ms": 85},
                      complete_attack_requests=11, pending_attack_requests=0,
                      expired_attack_requests=0, interrupted_attack_requests=0)
        result = attack_response(report)
        self.assertEqual(result["first_frame"], {"count": 11, "p95_ms": 20})
        self.assertEqual(result["host_action"], {"count": 11, "p95_ms": 85})
        self.assertEqual(result["requests"], {"complete": 11, "pending": 0,
                                            "expired": 0, "interrupted": 0})
        self.assertEqual(evaluate(report, 75), failures)

    def test_mixed_combat_keeps_predicted_and_corrected_native_families_distinct(self):
        shown = {"action_family": "throw", "generation": 7, "elapsed_ms": 16}
        host = {"action_family": "melee", "generation": 8, "elapsed_ms": 90}
        report = {"throw_diagnostics": {"requests": [
            {"tick": 100, "seat": 1, "first_attack_frame": shown,
             "host_attack_action": host}]}}
        request = throw_diagnostics(report)["requests"][0]
        self.assertEqual(request["first_attack_frame"], shown)
        self.assertEqual(request["host_attack_action"], host)

    def combat_report(self):
        report = self.active_stage_report()
        records = []
        for index in range(4):
            generation, tick = 10 + 10 * index, 100 + 90 * index
            seed = {"generation": generation, "action_family": "idle"}
            records.append({"epoch": 2, "tick": tick, "grant": 1, "seat": 1,
                            "shown_seed": dict(seed), "host_seed": dict(seed), "closed": True,
                            "interrupted": False,
                            "first_attack_frame": {"generation": generation + 1, "action": 55,
                                                   "action_family": "melee", "elapsed_ms": 25},
                            "host_attack_action": {"generation": generation + 1, "action": 55,
                                                   "action_family": "melee", "elapsed_ms": 85,
                                                   "actual_input_tick": tick, "tick": tick + 2,
                                                   "rollback": True}})
        report.update(attack_probe_schema=1, timing_scope="visual-submit-no-audio",
                      attack_observation_scope="not an input or hit ACK",
                      attack_to_first_frame={"count": 4, "p95_ms": 25},
                      attack_to_host_action={"count": 4, "p95_ms": 85},
                      complete_attack_requests=4, pending_attack_requests=0,
                      expired_attack_requests=0, interrupted_attack_requests=0,
                      throw_diagnostics={"omitted_requests": 0, "requests": records})
        # All four native requests chose melee: neither an animation of a throw
        # nor a projectile should be fabricated to satisfy this separate mode.
        report.update(missed_throws=4, expired_throw_authority=4)
        for key in ("throw_to_first_frame", "throw_to_authority",
                    "release_to_first_projectile_frame", "release_to_host_projectile"):
            report[key] = {"count": 0}
        return report

    def test_combat_mode_requires_explicit_opt_in_and_does_not_relabel_throw_failures(self):
        report = self.combat_report()
        unchanged = deepcopy(report)
        self.assertTrue(evaluate(report, 75))
        self.assertEqual(evaluate(report, 75, attack_workload="combat"), [])
        self.assertEqual(evaluate(report, None, attack_workload="combat"), [])
        self.assertEqual(report, unchanged)
        self.assertTrue(evaluate(report, 75, attack_workload="unknown"))

    def test_combat_mode_requires_each_unique_new_drawn_and_host_action(self):
        for field in ("first_attack_frame", "host_attack_action"):
            for value in (None, {}, [], "throw"):
                report = self.combat_report()
                report["throw_diagnostics"]["requests"][1][field] = value
                self.assertTrue(evaluate(report, None, attack_workload="combat"), (field, value))
            for name, value in (("generation", 20), ("generation", 0), ("generation", True),
                                ("action_family", "other"), ("action_family", "idle"),
                                ("elapsed_ms", -1), ("elapsed_ms", float("nan")),
                                ("elapsed_ms", float("inf")), ("elapsed_ms", 1001)):
                report = self.combat_report()
                report["throw_diagnostics"]["requests"][1][field][name] = value
                self.assertTrue(evaluate(report, None, attack_workload="combat"), (field, name, value))
        report = self.combat_report()
        report["throw_diagnostics"]["requests"][1] = deepcopy(report["throw_diagnostics"]["requests"][0])
        self.assertTrue(evaluate(report, None, attack_workload="combat"))
        for field in ("first_attack_frame", "host_attack_action"):
            report = self.combat_report()
            rows = report["throw_diagnostics"]["requests"]
            rows[1][field]["generation"] = rows[0][field]["generation"]
            rows[1]["shown_seed" if field == "first_attack_frame" else "host_seed"]["generation"] = 1
            self.assertTrue(evaluate(report, None, attack_workload="combat"))

    def test_combat_mode_rejects_unknown_missing_and_saturated_request_accounting(self):
        for field in ("complete_attack_requests", "pending_attack_requests", "expired_attack_requests",
                      "interrupted_attack_requests", "attack_probe_schema", "timing_scope", "throw_diagnostics"):
            report = self.combat_report()
            del report[field]
            self.assertTrue(evaluate(report, 75, attack_workload="combat"), field)
        for field in ("pending_attack_requests", "expired_attack_requests", "interrupted_attack_requests"):
            report = self.combat_report()
            report[field] = 1
            self.assertTrue(evaluate(report, None, attack_workload="combat"))
        for value in (None, [], [None], [None] * 257):
            report = self.combat_report()
            report["throw_diagnostics"]["requests"] = value
            self.assertTrue(evaluate(report, None, attack_workload="combat"))
        report = self.combat_report()
        report["throw_diagnostics"]["omitted_requests"] = 1
        self.assertTrue(evaluate(report, None, attack_workload="combat"))
        for value in (None, -1, True, "4", 3):
            report = self.combat_report()
            report["complete_attack_requests"] = value
            self.assertTrue(evaluate(report, None, attack_workload="combat"))

    def test_combat_mode_preserves_ownership_idle_and_native_deadline_evidence(self):
        for field, value in (("epoch", 0), ("grant", 0), ("seat", 4), ("tick", -1), ("interrupted", True)):
            report = self.combat_report()
            report["throw_diagnostics"]["requests"][0][field] = value
            self.assertTrue(evaluate(report, None, attack_workload="combat"))
        report = self.combat_report()
        report["throw_diagnostics"]["requests"][0]["shown_seed"]["action_family"] = "melee"
        self.assertTrue(evaluate(report, None, attack_workload="combat"))
        for field, value in (("rollback", False), ("actual_input_tick", 99),
                             ("actual_input_tick", 103), ("tick", "102")):
            report = self.combat_report()
            report["throw_diagnostics"]["requests"][0]["host_attack_action"][field] = value
            self.assertTrue(evaluate(report, None, attack_workload="combat"))

    def test_combat_mode_checks_timing_and_cadence_without_requiring_a_melee_projectile(self):
        for field in ("attack_to_first_frame", "attack_to_host_action"):
            for value in ({"count": 0}, {"count": 3, "p95_ms": 25}, None,
                          {"count": 4, "p95_ms": float("nan")}):
                report = self.combat_report()
                report[field] = value
                self.assertTrue(evaluate(report, None, attack_workload="combat"))
        report = self.combat_report()
        report["attack_to_first_frame"]["p95_ms"] = 210
        report["throw_diagnostics"]["requests"][0]["first_attack_frame"]["elapsed_ms"] = 210
        self.assertTrue(evaluate(report, 75, attack_workload="combat"))
        self.assertTrue(evaluate(report, None, attack_workload="combat"))
        self.assertEqual(evaluate(report, 75, attack_workload="combat", attack_limit_ms=220), [])
        # A low aggregate cannot hide the actual slow per-request sample.
        report["attack_to_first_frame"]["p95_ms"] = 25
        self.assertTrue(evaluate(report, 75, attack_workload="combat"))
        for metric, field, value in (("sample_to_send", "p95_ms", 20),
                                      ("render_interval", "median_ms", 35),
                                      ("update_interval", "median_ms", 35)):
            report = self.combat_report()
            report[metric][field] = value
            self.assertTrue(evaluate(report, None, attack_workload="combat"))
        report = self.combat_report()
        report["motion_to_first_frame"]["p95_ms"] = 100
        self.assertTrue(evaluate(report, 75, attack_workload="combat"))
        self.assertEqual(evaluate(report, None, attack_workload="combat"), [])

    def test_combat_mode_accepts_native_family_corrections_without_claiming_a_hit(self):
        report = self.combat_report()
        report["throw_diagnostics"]["requests"][0]["first_attack_frame"].update(action_family="throw", action=9)
        self.assertEqual(evaluate(report, 75, attack_workload="combat"), [])

    def test_combat_cli_validates_mode_and_budgets_before_launch(self):
        for options in (["--attack-workload", "anything"],
                        ["--attack-workload", "combat", "--workload", "rapid-five"],
                        ["--attack-workload", "combat", "--workload", "turbo-B"],
                        ["--attack-workload", "combat", "--element", "fire"],
                        ["--attack-workload", "combat", "--max-attack-ms", "nan"],
                        ["--attack-workload", "combat", "--max-attack-ms", "0"]):
            with patch("netplay_latency.subprocess.Popen") as create:
                with self.assertRaises(SystemExit), redirect_stderr(StringIO()):
                    main(options)
                create.assert_not_called()

    def test_combat_cli_labels_distinct_verdict_and_retains_strict_throw_failures(self):
        guest = self.combat_report()
        host = dict(guest, role="host")
        with tempfile.TemporaryDirectory() as directory, redirect_stderr(StringIO()), \
                redirect_stdout(StringIO()) as output, \
                patch("netplay_latency.pathlib.Path.is_file", return_value=True), \
                patch("netplay_latency.launch", return_value=(object(), Path(directory) / "unused")), \
                patch("netplay_latency.wait_invitation"), \
                patch("netplay_latency.read_report", side_effect=[guest, host]):
            result = main(["--stage", "G1", "--attack-workload", "combat", "--prediction", "on",
                           "--seconds", "8", "--build-dir", directory])
            self.assertEqual(result, 0)
            self.assertIn('"run": "combat-60fps-on"', output.getvalue())
            self.assertIn('"attack_workload": "combat"', output.getvalue())
            self.assertIn('"strict_throw_failures": ["No samples', output.getvalue())
            self.assertIn("no hit or projectile guarantee", output.getvalue())

    def report(self):
        return {"schema": 3, "status": "complete", "role": "guest", "missed_motion": 0,
                "stage": "tower", "loaded_stage": "tower", "scene_id": 3585,
                "stage_probe_schema": 1, "stage_activity_scope": "authority-checkpoints",
                "stage_authority_observations": 100, "stage_enemy_frames": 0,
                "stage_enemy_motion_samples": 0, "stage_enemy_animation_samples": 0,
                "stage_enemy_peak": 0, "stage_opponent_seed_observations": 100,
                "rtt": {"count": 10}, "sample_to_send": {"count": 10, "p95_ms": 0.5},
                "sample_to_resolved": {"count": 10},
                "render_fps": 60, "render_interval": {"count": 10, "median_ms": 16.7},
                "update_interval": {"count": 10, "median_ms": 16.7},
                "motion_to_first_frame": {"count": 4, "p95_ms": 25},
                "throw_to_first_frame": {"count": 4, "p95_ms": 120}, "missed_throws": 0,
                "throw_to_authority": {"count": 4, "p95_ms": 100},
                "release_to_first_projectile_frame": {"count": 4, "p95_ms": 17},
                "release_to_host_projectile": {"count": 4, "p95_ms": 80},
                "pending_throw_authority": 0, "expired_throw_authority": 0,
                "interrupted_throw_authority": 0, "superseded_throw_authority": 0,
                "pending_releases": 0, "expired_releases": 0, "interrupted_releases": 0,
                "unobserved_host_projectiles": 0}

    def test_requires_evidence(self):
        self.assertTrue(evaluate({}, 75))
        report = self.report()
        report["rtt"] = {"count": 0}
        self.assertTrue(evaluate(report, 75))

    def active_stage_report(self):
        report = self.report()
        report.update(stage="G1", loaded_stage="G1", scene_id=257, stage_enemy_peak=8,
                      stage_enemy_frames=90, stage_enemy_motion_samples=240,
                      stage_enemy_animation_samples=300)
        return report

    def test_native_stage_requires_exact_loaded_identity_and_observations(self):
        for report in (self.report(), self.active_stage_report()):
            self.assertEqual(evaluate(report, 75), [])
            self.assertEqual(evaluate(report, None), [])
            for key in ("stage", "loaded_stage", "scene_id", "stage_probe_schema",
                        "stage_activity_scope", "stage_authority_observations",
                        "stage_enemy_frames", "stage_enemy_motion_samples", "stage_enemy_animation_samples",
                        "stage_enemy_peak", "stage_opponent_seed_observations"):
                invalid = dict(report)
                del invalid[key]
                self.assertTrue(evaluate(invalid, 75), key)
        for value in (None, False, 0, "G2", [], {}):
            report = self.report()
            report["stage"] = value
            self.assertTrue(evaluate_stage(report))
        for value in (None, True, False, 0, -1, 65536, "257"):
            report = self.active_stage_report()
            report["scene_id"] = value
            self.assertTrue(evaluate_stage(report))
        report = self.active_stage_report()
        report["loaded_stage"] = "tower"
        self.assertIn("Loaded native stage differs from reported stage", evaluate_stage(report))
        self.assertIn("Report stage differs from requested stage", evaluate_stage(report, "tower"))

    def test_active_stage_cannot_be_certified_by_empty_or_frozen_enemies(self):
        for key in ("stage_enemy_peak", "stage_enemy_frames", "stage_enemy_motion_samples"):
            report = self.active_stage_report()
            report[key] = 0
            self.assertIn("G1 run lacks moving native enemy observations", evaluate(report, None))
        for key in ("stage_authority_observations", "stage_enemy_frames", "stage_enemy_motion_samples",
                    "stage_enemy_animation_samples", "stage_enemy_peak", "stage_opponent_seed_observations"):
            for value in (None, True, False, -1, 1.0, "8"):
                report = self.active_stage_report()
                report[key] = value
                self.assertIn(f"No valid counter for {key}", evaluate_stage(report))
        for key, value in (("stage_enemy_peak", 129), ("stage_authority_observations", 0),
                           ("stage_enemy_frames", 101), ("stage_opponent_seed_observations", 101)):
            report = self.active_stage_report()
            report[key] = value
            self.assertTrue(evaluate_stage(report))
        report = self.active_stage_report()
        report["stage_opponent_seed_observations"] = 0
        # An omitted optional replay supplement is measured, not invented or
        # mistaken for absent ordinary enemies in the actual native stage.
        self.assertEqual(evaluate_stage(report), [])

    def test_active_stage_does_not_relax_latency_or_real_action_evidence(self):
        report = self.active_stage_report()
        report["motion_to_first_frame"]["p95_ms"] = 100
        self.assertTrue(evaluate(report, 75))
        self.assertEqual(evaluate(report, None), [])
        report["release_to_host_projectile"]["count"] = 0
        self.assertTrue(evaluate(report, None))

    def test_stage_parser_and_launcher_preserve_native_default(self):
        with patch("netplay_latency.subprocess.Popen") as create:
            with self.assertRaises(SystemExit), redirect_stderr(StringIO()):
                main(["--stage", "G2"])
            create.assert_not_called()
        for stage in (None, "tower", "G1"):
            args = SimpleNamespace(seconds=20, players=2, workload="throw", element="none",
                                   internet=False, assets=None, data=None)
            if stage is not None:
                args.stage = stage
            with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
                output = Path(directory)
                with patch("netplay_latency.subprocess.Popen") as create:
                    create.return_value.poll.return_value = 0
                    launch(stack, output / "probe", "guest", output / "invitation", output,
                           args, 60, "on")
                    command = create.call_args.args[0]
                    self.assertEqual(command[command.index("--stage") + 1], stage or "tower")

    def test_pair_reports_must_match_requested_stage_and_each_other(self):
        for role in ("host", "guest"):
            for field, value in (("stage", "tower"), ("loaded_stage", "tower"), ("scene_id", 3585)):
                guest = self.active_stage_report()
                host = dict(guest, role="host")
                (host if role == "host" else guest)[field] = value
                with tempfile.TemporaryDirectory() as directory, redirect_stderr(StringIO()) as error, \
                        redirect_stdout(StringIO()), patch("netplay_latency.pathlib.Path.is_file", return_value=True), \
                        patch("netplay_latency.launch", return_value=(object(), Path(directory) / "unused")), \
                        patch("netplay_latency.wait_invitation"), \
                        patch("netplay_latency.read_report", side_effect=[guest, host]):
                    result = main(["--stage", "G1", "--prediction", "on", "--seconds", "8",
                                   "--build-dir", directory])
                    self.assertEqual(result, 1)
                    self.assertIn("stage" if field != "scene_id" else "scene IDs differ", error.getvalue())

    def test_rapid_five_requires_real_complete_fan_observations(self):
        report = self.report()
        report.update(workload="rapid-five", item_probe_schema=1,
                      projectile_timing_scope="center-shot", certified_item_observations=50,
                      displayed_complete_fans=4, host_complete_fans=4,
                      displayed_partial_fans=0, host_partial_fans=0)
        self.assertEqual(evaluate(report, 75), [])
        for field in ("item_probe_schema", "projectile_timing_scope", "certified_item_observations",
                      "displayed_complete_fans", "host_complete_fans",
                      "displayed_partial_fans", "host_partial_fans"):
            missing = dict(report)
            del missing[field]
            self.assertTrue(evaluate(missing, 75), field)
        for field in ("displayed_complete_fans", "host_complete_fans"):
            partial = dict(report)
            partial[field] = 2
            self.assertTrue(evaluate(partial, None), field)
        for field in ("displayed_partial_fans", "host_partial_fans"):
            partial = dict(report)
            partial[field] = 1
            self.assertTrue(evaluate(partial, 75), field)

    def speed_report(self):
        report = self.report()
        report.update(workload="speed-rapid-five", item_probe_schema=1, speed_probe_schema=1,
                      timing_scope="visual-submit-no-audio", projectile_timing_scope="center-shot",
                      certified_item_observations=50, displayed_complete_fans=4,
                      host_complete_fans=4, displayed_partial_fans=0, host_partial_fans=0)
        for prefix in ("displayed_", "host_"):
            report[prefix + "speed_certified_observations"] = 50
            report[prefix + "speed_motion_observations"] = 12
            report[prefix + "speed_minimum_pace"] = 8.5
            report[prefix + "speed_maximum_pace"] = 8.5
        return report

    def test_speed_workload_requires_certified_local_and_authoritative_movement(self):
        report = self.speed_report()
        self.assertEqual(evaluate(report, 75), [])
        self.assertEqual(evaluate(report, None), [])
        for field in ("speed_probe_schema", "timing_scope"):
            missing = dict(report)
            del missing[field]
            self.assertTrue(evaluate(missing, 75), field)
        for prefix in ("displayed_", "host_"):
            for suffix in ("speed_certified_observations", "speed_motion_observations"):
                field = prefix + suffix
                for value in (None, False, True, 0, 2, "12"):
                    with self.subTest(field=field, value=value):
                        missing = dict(report)
                        missing[field] = value
                        self.assertTrue(evaluate(missing, 75))
                        self.assertTrue(evaluate(missing, None))

    def test_speed_workload_cannot_pass_with_stale_unboosted_or_invalid_pace(self):
        for prefix in ("displayed_", "host_"):
            for suffix in ("speed_minimum_pace", "speed_maximum_pace"):
                field = prefix + suffix
                for value in (None, False, True, 5, 7.99, 12.51, float("nan"), float("inf"), "8.5"):
                    with self.subTest(field=field, value=value):
                        report = self.speed_report()
                        report[field] = value
                        self.assertTrue(evaluate(report, 75))
                        self.assertTrue(evaluate(report, None))
            report = self.speed_report()
            report[prefix + "speed_minimum_pace"] = 9
            self.assertTrue(evaluate(report, 75))  # min cannot exceed max
        report = self.speed_report()
        report["displayed_speed_minimum_pace"] = 8
        report["host_speed_maximum_pace"] = 12.5
        self.assertEqual(evaluate(report, 75), [])

    def test_speed_evidence_does_not_replace_fan_or_latency_evidence(self):
        for field in ("item_probe_schema", "certified_item_observations", "displayed_complete_fans",
                      "host_complete_fans", "motion_to_first_frame", "throw_to_authority",
                      "release_to_first_projectile_frame", "release_to_host_projectile"):
            report = self.speed_report()
            del report[field]
            self.assertTrue(evaluate(report, 75), field)
        report = self.speed_report()
        report["motion_to_first_frame"]["p95_ms"] = 100
        self.assertTrue(evaluate(report, 75))
        self.assertEqual(evaluate(report, None), [])
        report["displayed_partial_fans"] = 1
        self.assertTrue(evaluate(report, None))

    def test_speed_workload_cli_accepts_ordinary_probe_duration_without_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            error = StringIO()
            with redirect_stderr(error):
                result = main(["--workload", "speed-rapid-five", "--seconds", "8",
                               "--build-dir", directory])
            self.assertEqual(result, 1)
            self.assertIn("netplayprobe is missing", error.getvalue())

    def element_report(self, element="fire", workload="throw"):
        report = self.speed_report() if workload == "speed-rapid-five" else self.report()
        report.update(workload=workload, element=element, element_probe_schema=1,
                      element_flags={"fire": 1, "lightning": 2, "light": 3, "acid": 4}[element],
                      timing_scope="visual-submit-no-audio", element_certified_observations=50,
                      displayed_element_projectiles=4, host_element_projectiles=4,
                      displayed_invalid_element_projectiles=0, host_invalid_element_projectiles=0)
        if workload == "rapid-five":
            report.update(item_probe_schema=1, projectile_timing_scope="center-shot",
                          certified_item_observations=50, displayed_complete_fans=4,
                          host_complete_fans=4, displayed_partial_fans=0, host_partial_fans=0)
        return report

    def test_four_elemental_amulets_compose_with_existing_throw_workloads(self):
        for element in ("fire", "lightning", "light", "acid"):
            for workload in ("throw", "rapid-five", "speed-rapid-five"):
                with self.subTest(element=element, workload=workload):
                    report = self.element_report(element, workload)
                    self.assertEqual(evaluate(report, 75), [])
                    self.assertEqual(evaluate(report, None), [])
                    with tempfile.TemporaryDirectory() as directory, redirect_stderr(StringIO()) as error:
                        self.assertEqual(main(["--element", element, "--workload", workload,
                                               "--seconds", "8", "--build-dir", directory]), 1)
                        self.assertIn("netplayprobe is missing", error.getvalue())

    def test_elemental_evidence_requires_matching_resource_bearing_native_releases(self):
        for field in ("element_probe_schema", "element_flags", "timing_scope",
                      "element_certified_observations", "displayed_element_projectiles",
                      "host_element_projectiles", "displayed_invalid_element_projectiles",
                      "host_invalid_element_projectiles"):
            report = self.element_report()
            del report[field]
            self.assertTrue(evaluate(report, 75), field)
        for field in ("element_certified_observations", "displayed_element_projectiles",
                      "host_element_projectiles"):
            for value in (None, False, True, 0, 1, 2, "4"):
                report = self.element_report()
                report[field] = value
                self.assertTrue(evaluate(report, None), (field, value))
        for field in ("displayed_invalid_element_projectiles", "host_invalid_element_projectiles"):
            for value in (None, False, True, -1, 1, "0"):
                report = self.element_report()
                report[field] = value
                self.assertTrue(evaluate(report, 75), (field, value))
        for field in ("element_probe_schema", "element_flags"):
            for value in (None, False, True, 0, 2, "1"):
                report = self.element_report()
                report[field] = value
                self.assertTrue(evaluate(report, 75), (field, value))

    def test_elemental_counters_do_not_substitute_for_projectile_latency_or_fans(self):
        for field in ("release_to_first_projectile_frame", "release_to_host_projectile",
                      "throw_to_authority", "displayed_complete_fans", "host_complete_fans"):
            report = self.element_report("acid", "rapid-five")
            del report[field]
            self.assertTrue(evaluate(report, 75), field)
        report = self.element_report()
        report["release_to_first_projectile_frame"]["p95_ms"] = 100
        self.assertTrue(evaluate(report, 75))
        self.assertEqual(evaluate(report, None), [])
        report["unobserved_host_projectiles"] = 1
        self.assertTrue(evaluate(report, None))

    def test_invalid_elemental_combinations_fail_before_launch(self):
        for element in (None, [], 1, "ice", "", False):
            report = self.element_report()
            report["element"] = element
            self.assertTrue(evaluate(report, 75), element)
        for workload in ("turbo-B", "turbo-C", "shove"):
            with self.assertRaises(SystemExit), redirect_stderr(StringIO()):
                main(["--element", "fire", "--workload", workload, "--seconds", "20"])
        with self.assertRaises(SystemExit), redirect_stderr(StringIO()):
            main(["--element", "ice"])

    def test_launch_forwards_element_and_does_not_change_default_workload(self):
        for element in ("none", "fire", "lightning", "light", "acid"):
            args = SimpleNamespace(seconds=20, players=2, workload="throw", element=element,
                                   internet=False, assets=None, data=None)
            with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
                output = Path(directory)
                with patch("netplay_latency.subprocess.Popen") as create:
                    create.return_value.poll.return_value = 0
                    launch(stack, output / "probe", "guest", output / "invitation", output,
                           args, 60, "on")
                    command = create.call_args.args[0]
                    self.assertEqual(command[command.index("--element") + 1], element)
                    self.assertEqual(command[command.index("--workload") + 1], "throw")

    def test_pair_reports_must_match_requested_element_and_workload(self):
        for role in ("host", "guest"):
            for field, value in (("element", "light"), ("workload", "rapid-five")):
                guest = self.element_report()
                host = dict(guest, role="host")
                (host if role == "host" else guest)[field] = value
                with tempfile.TemporaryDirectory() as directory, redirect_stderr(StringIO()) as error, \
                        redirect_stdout(StringIO()), patch("netplay_latency.pathlib.Path.is_file", return_value=True), \
                        patch("netplay_latency.launch", return_value=(object(), Path(directory) / "unused")), \
                        patch("netplay_latency.wait_invitation"), \
                        patch("netplay_latency.read_report", side_effect=[guest, host]):
                    result = main(["--element", "fire", "--prediction", "on", "--seconds", "8",
                                   "--build-dir", directory])
                    self.assertEqual(result, 1)
                    self.assertIn("loadout differs from requested configuration", error.getvalue())

    def test_latency_budget_detects_eventually_correct_but_sluggish_response(self):
        report = self.report()
        self.assertEqual(evaluate(report, 75), [])
        report["motion_to_first_frame"]["p95_ms"] = 200
        self.assertTrue(evaluate(report, 75))
        self.assertEqual(evaluate(report, None), [])  # unpredicted A/B baseline

    def test_a_missing_response_is_not_a_fast_response(self):
        report = self.report()
        report["missed_motion"] = 1
        self.assertTrue(evaluate(report, 75))

    def test_requested_framerate_is_not_evidence_of_achieved_framerate(self):
        report = self.report()
        report["render_interval"]["median_ms"] = 31
        self.assertTrue(evaluate(report, 75))

    def test_fast_prediction_cannot_hide_slow_or_missing_attacks(self):
        report = self.report()
        report["throw_to_first_frame"]["p95_ms"] = 260
        self.assertTrue(evaluate(report, 75))
        self.assertEqual(evaluate(report, 75, 300), [])
        report["missed_throws"] = 1
        self.assertTrue(evaluate(report, 75, 300))
        del report["throw_to_first_frame"]
        self.assertTrue(evaluate(report, 75, 300))

    def test_queue_budget_catches_an_extra_application_tick(self):
        report = self.report()
        report["sample_to_send"]["p95_ms"] = 16.7
        self.assertTrue(evaluate(report, 75))
        self.assertEqual(evaluate(report, 75, send_limit_ms=20), [])

    def test_local_action_startup_cannot_hide_missing_host_confirmation(self):
        report = self.report()
        report["throw_to_first_frame"]["p95_ms"] = 5
        del report["throw_to_authority"]
        self.assertIn("No samples for throw_to_authority", evaluate(report, 75))
        for counter in ("pending_throw_authority", "expired_throw_authority",
                        "interrupted_throw_authority", "superseded_throw_authority"):
            report = self.report()
            report[counter] = 1
            self.assertIn(f"{counter}: 1", evaluate(report, 75))

    def test_a_single_confirmed_throw_is_insufficient_evidence(self):
        report = self.report()
        report["throw_to_authority"]["count"] = 1
        self.assertIn("Fewer than three host-confirmed throws", evaluate(report, 75))

    def test_authority_counters_must_be_present_nonnegative_integers(self):
        for key in ("pending_throw_authority", "expired_throw_authority",
                    "interrupted_throw_authority", "superseded_throw_authority"):
            report = self.report()
            del report[key]
            self.assertIn(f"No valid counter for {key}", evaluate(report, 75))
            self.assertTrue(evaluate(report, None))
            for value in (None, -1, True, False, 0.0, "0", float("nan")):
                with self.subTest(key=key, value=value):
                    report = self.report()
                    report[key] = value
                    self.assertIn(f"No valid counter for {key}", evaluate(report, 75))
                    self.assertTrue(evaluate(report, None))

    def test_ordinary_timings_reject_invalid_values_even_without_prediction_budget(self):
        for key in ("sample_to_send", "motion_to_first_frame", "throw_to_first_frame",
                    "throw_to_authority", "release_to_first_projectile_frame", "release_to_host_projectile"):
            for value in (None, -1, float("nan"), float("inf"), "17", True, False, [], {}):
                with self.subTest(key=key, value=value):
                    report = self.report()
                    report[key]["p95_ms"] = value
                    self.assertIn(f"No valid 95th-percentile timing for {key}", evaluate(report, 75))
                    self.assertTrue(evaluate(report, None))
            report = self.report()
            del report[key]["p95_ms"]
            self.assertTrue(evaluate(report, 75))
            report = self.report()
            report[key]["p95_ms"] = 0
            self.assertEqual(evaluate(report, 75), [])

    def test_ordinary_cadence_rejects_invalid_or_missing_median(self):
        for key in ("update_interval", "render_interval"):
            for value in (None, -1, float("nan"), float("inf"), "17", True, False, [], {}):
                with self.subTest(key=key, value=value):
                    report = self.report()
                    report[key]["median_ms"] = value
                    self.assertIn(f"No valid median timing for {key}", evaluate(report, 75))
                    self.assertTrue(evaluate(report, None))
            report = self.report()
            del report[key]["median_ms"]
            self.assertTrue(evaluate(report, 75))

    def test_malformed_ordinary_distributions_fail_instead_of_crashing(self):
        for key in ("rtt", "sample_to_send", "sample_to_resolved", "motion_to_first_frame",
                    "throw_to_first_frame", "throw_to_authority", "update_interval", "render_interval",
                    "release_to_first_projectile_frame", "release_to_host_projectile"):
            for value in (None, True, 5, "17", []):
                with self.subTest(key=key, value=value):
                    report = self.report()
                    report[key] = value
                    self.assertIn(f"No samples for {key}", evaluate(report, 75))
                    self.assertTrue(evaluate(report, None))
            for count in (None, True, -1, "4", 4.0):
                with self.subTest(key=key, count=count):
                    report = self.report()
                    report[key]["count"] = count
                    self.assertIn(f"No samples for {key}", evaluate(report, 75))

    def test_projectile_evidence_requires_schema_and_multiple_valid_observations(self):
        report = self.report()
        report["schema"] = 2
        self.assertIn("No projectile-aware telemetry schema 3", evaluate(report, 75))
        for key in ("release_to_first_projectile_frame", "release_to_host_projectile"):
            for count in (0, 1, 2):
                with self.subTest(key=key, count=count):
                    report = self.report()
                    report[key]["count"] = count
                    self.assertIn(f"Fewer than three observations for {key}", evaluate(report, 75))
                    self.assertTrue(evaluate(report, None))  # baseline still needs evidence
            report = self.report()
            del report[key]
            self.assertIn(f"No samples for {key}", evaluate(report, 75))
            report = self.report()
            report[key]["count"] = 3
            self.assertEqual(evaluate(report, 75), [])

    def test_release_budget_cannot_be_satisfied_by_fast_animation_alone(self):
        report = self.report()
        report["throw_to_first_frame"]["p95_ms"] = 1
        report["release_to_first_projectile_frame"]["p95_ms"] = 100
        self.assertIn("95th-percentile native release-to-projectile draw exceeds 75 ms",
                      evaluate(report, 75))
        self.assertEqual(evaluate(report, 75, release_limit_ms=110), [])
        self.assertEqual(evaluate(report, None), [])
        self.assertEqual(evaluate(report, None, release_limit_ms=None), [])

    def test_projectile_observation_failures_are_rejected_with_and_without_prediction(self):
        for key in ("pending_releases", "expired_releases", "interrupted_releases",
                    "unobserved_host_projectiles"):
            with self.subTest(key=key):
                report = self.report()
                report[key] = 1
                self.assertIn(f"{key}: 1", evaluate(report, 75))
                self.assertIn(f"{key}: 1", evaluate(report, None))
                del report[key]
                self.assertIn(f"No valid counter for {key}", evaluate(report, 75))

    def test_projectile_timings_must_be_finite_even_without_a_latency_budget(self):
        for key in ("release_to_first_projectile_frame", "release_to_host_projectile"):
            for value in (None, -1, float("nan"), float("inf"), "17", True):
                with self.subTest(key=key, value=value):
                    report = self.report()
                    report[key]["p95_ms"] = value
                    self.assertIn(f"No valid 95th-percentile timing for {key}",
                                  evaluate(report, 75))
                    self.assertTrue(evaluate(report, None))

    def test_release_budget_validation_precedes_any_probe_launch(self):
        for value in ("0", "-1", "nan", "inf"):
            with self.subTest(value=value):
                with self.assertRaises(SystemExit), redirect_stderr(StringIO()):
                    main(["--max-release-ms", value])

    def test_pair_mode_cannot_remove_a_user_supplied_invitation(self):
        with tempfile.TemporaryDirectory() as directory:
            invitation = Path(directory) / "private.txt"
            invitation.write_text("private ticket", encoding="utf-8")
            with self.assertRaises(SystemExit), redirect_stderr(StringIO()):
                main(["--invitation", str(invitation)])
            self.assertEqual(invitation.read_text(encoding="utf-8"), "private ticket")

    def turbo_report(self):
        report = self.report()
        report.update({"workload": "turbo-B", "turbo_probe_schema": 1,
                       "turbo_to_first_frame": {"count": 4, "p95_ms": 25},
                       "turbo_to_host_action": {"count": 4, "p95_ms": 100},
                       "turbo_requests": 4, "complete_turbo_requests": 4,
                       "pending_turbo_requests": 0, "expired_turbo_requests": 0,
                       "interrupted_turbo_requests": 0, "omitted_turbo_requests": 0})
        for key in ("throw_to_first_frame", "throw_to_authority", "missed_throws",
                    "release_to_first_projectile_frame", "release_to_host_projectile",
                    "pending_releases", "expired_releases", "interrupted_releases",
                    "unobserved_host_projectiles", "motion_to_first_frame", "missed_motion"):
            del report[key]
        return report

    def test_turbo_workload_needs_action_not_ordinary_projectile_evidence(self):
        for workload in ("turbo-B", "turbo-C", "shove"):
            with self.subTest(workload=workload):
                report = self.turbo_report()
                report["workload"] = workload
                self.assertEqual(evaluate(report, 75), [])
                self.assertEqual(evaluate(report, None), [])
        report["workload"] = "unknown"
        self.assertIn("Unknown turbo workload", evaluate(report, 75))

    def test_turbo_local_animation_cannot_replace_host_action_observation(self):
        for key in ("turbo_to_first_frame", "turbo_to_host_action"):
            for count in (0, 1, 2, True):
                with self.subTest(key=key, count=count):
                    report = self.turbo_report()
                    report[key]["count"] = count
                    self.assertIn(f"Fewer than three observations for {key}", evaluate(report, 75))
            report = self.turbo_report()
            del report[key]
            self.assertTrue(evaluate(report, 75))
            self.assertTrue(evaluate(report, None))

    def test_turbo_counts_cannot_hide_missed_or_incomplete_requests(self):
        for key in ("pending_turbo_requests", "expired_turbo_requests", "interrupted_turbo_requests",
                    "omitted_turbo_requests"):
            with self.subTest(key=key):
                report = self.turbo_report()
                report[key] = 1
                self.assertIn(f"{key}: 1", evaluate(report, 75))
                self.assertIn(f"{key}: 1", evaluate(report, None))
                del report[key]
                self.assertIn(f"No valid counter for {key}", evaluate(report, 75))
        for key in ("turbo_requests", "complete_turbo_requests"):
            report = self.turbo_report()
            report[key] = 3
            self.assertTrue(evaluate(report, 75))
        report = self.turbo_report()
        del report["turbo_probe_schema"]
        self.assertTrue(evaluate(report, 75))

    def test_turbo_latency_budget_and_finite_timing(self):
        report = self.turbo_report()
        report["turbo_to_first_frame"]["p95_ms"] = 100
        self.assertIn("95th-percentile turbo startup-to-draw exceeds 75 ms", evaluate(report, 75))
        self.assertEqual(evaluate(report, None), [])
        self.assertEqual(evaluate(report, 75, turbo_limit_ms=110), [])
        for key in ("turbo_to_first_frame", "turbo_to_host_action"):
            for value in (None, -1, float("nan"), float("inf"), "17", True):
                with self.subTest(key=key, value=value):
                    report = self.turbo_report()
                    report[key]["p95_ms"] = value
                    self.assertTrue(evaluate(report, 75))
                    self.assertTrue(evaluate(report, None))

    def test_turbo_cannot_claim_a_target_cadence_without_measuring_it(self):
        for key in ("update_interval", "render_interval"):
            for value in (None, -1, float("nan"), float("inf"), "17", True, 100):
                with self.subTest(key=key, value=value):
                    report = self.turbo_report()
                    report[key]["median_ms"] = value
                    self.assertTrue(evaluate(report, 75))
        report = self.turbo_report()
        report["sample_to_send"]["p95_ms"] = 20
        self.assertTrue(evaluate(report, 75))

    def test_turbo_budget_and_duration_validate_before_launch(self):
        for value in ("0", "-1", "nan", "inf"):
            with self.subTest(value=value):
                with self.assertRaises(SystemExit), redirect_stderr(StringIO()):
                    main(["--workload", "turbo-B", "--max-turbo-ms", value])
        with self.assertRaises(SystemExit), redirect_stderr(StringIO()):
            main(["--workload", "turbo-C", "--seconds", "8"])

    def test_shove_requires_both_observations_and_complete_accounting(self):
        report = self.turbo_report()
        report["workload"] = "shove"
        self.assertEqual(evaluate(report, 75), [])
        report["turbo_to_host_action"]["count"] = 0
        self.assertTrue(evaluate(report, 75))
        self.assertTrue(evaluate(report, None))
        report["turbo_to_host_action"]["count"] = 4
        report["interrupted_turbo_requests"] = 1
        self.assertTrue(evaluate(report, 75))
        with self.assertRaises(SystemExit), redirect_stderr(StringIO()):
            main(["--workload", "shove", "--seconds", "8"])

    def volley_report(self):
        report = self.turbo_report()
        base = self.report()
        for key in ("release_to_first_projectile_frame", "release_to_host_projectile",
                    "pending_releases", "expired_releases", "interrupted_releases",
                    "unobserved_host_projectiles"):
            report[key] = base[key]
        report.update(character=3, require_volleys=True, turbo_volley_probe_schema=1,
                      projectile_timing_scope="turbo-volley-release", timing_scope="visual-submit-no-audio",
                      displayed_volley_projectiles=4, host_volley_projectiles=4, matched_volley_projectiles=4,
                      displayed_invalid_volley_projectiles=0, host_invalid_volley_projectiles=0,
                      displayed_volley_overflow=False, host_volley_overflow=False)
        report["release_samples"] = [dict(epoch=1, tick=50, generation=3, grant=1, seat=1,
                                           strong=False, fan_ordinal=0, emission=(slot << 16) | index,
                                           first_frame_ms=17, host_ms=80, outcome="complete")
                                     for slot in (1, 2) for index in (0, 1)]
        return report

    def test_required_volleys_measure_projectiles_separately_from_startup(self):
        for workload in ("turbo-B", "turbo-C"):
            report = self.volley_report()
            report["workload"] = workload
            self.assertEqual(evaluate(report, 75), [])
            self.assertEqual(evaluate(report, None), [])
            report["release_to_first_projectile_frame"]["p95_ms"] = 100
            self.assertIn("95th-percentile native release-to-projectile draw exceeds 75 ms",
                          evaluate(report, 75))
            self.assertEqual(evaluate(report, 75, release_limit_ms=110), [])
            self.assertEqual(evaluate(report, None), [])
            report["unobserved_host_projectiles"] = 1
            self.assertIn("unobserved_host_projectiles: 1", evaluate(report, None))

    def test_turbo_action_alone_cannot_pass_a_required_volley_run(self):
        report = self.turbo_report()
        report["require_volleys"] = True
        self.assertTrue(evaluate(report, 75))
        self.assertTrue(evaluate(report, None))
        for key in ("turbo_volley_probe_schema", "projectile_timing_scope", "timing_scope",
                    "displayed_volley_projectiles", "host_volley_projectiles", "matched_volley_projectiles",
                    "displayed_invalid_volley_projectiles", "host_invalid_volley_projectiles",
                    "displayed_volley_overflow", "host_volley_overflow", "release_samples",
                    "release_to_first_projectile_frame", "release_to_host_projectile",
                    "pending_releases", "expired_releases", "interrupted_releases",
                    "unobserved_host_projectiles"):
            with self.subTest(key=key):
                report = self.volley_report()
                del report[key]
                self.assertTrue(evaluate(report, None))

    def test_required_volley_evidence_rejects_missing_nonfinite_and_saturated_samples(self):
        for key in ("displayed_volley_projectiles", "host_volley_projectiles", "matched_volley_projectiles"):
            for value in (None, False, True, -1, 0, 2, "4"):
                report = self.volley_report()
                report[key] = value
                self.assertTrue(evaluate(report, None), (key, value))
        for prefix in ("displayed_", "host_"):
            for value in (None, 0, 1, True, "false"):
                report = self.volley_report()
                report[prefix + "volley_overflow"] = value
                self.assertTrue(evaluate(report, None))
            for value in (None, -1, True, False, "0", 1):
                report = self.volley_report()
                report[prefix + "invalid_volley_projectiles"] = value
                self.assertTrue(evaluate(report, None))
        for key in ("release_to_first_projectile_frame", "release_to_host_projectile"):
            for value in (None, False, True, -1, float("inf"), float("nan"), "17"):
                report = self.volley_report()
                report[key]["p95_ms"] = value
                self.assertTrue(evaluate(report, None), (key, value))
            for count in (None, False, True, 0, 2, "4"):
                report = self.volley_report()
                report[key]["count"] = count
                self.assertTrue(evaluate(report, None))
            report = self.volley_report()
            report[key] = "malformed"
            self.assertTrue(evaluate(report, None))

    def test_turbo_release_sample_identity_is_not_a_frame_counter_or_ordinary_throw(self):
        for key, value in (("emission", 0), ("emission", 0x810000), ("emission", -1),
                           ("emission", True), ("strong", True), ("fan_ordinal", 1),
                           ("seat", 4), ("grant", 0), ("generation", 0), ("epoch", 0),
                           ("tick", -1), ("tick", 1 << 64), ("grant", 1 << 32),
                           ("outcome", "pending"), ("first_frame_ms", None),
                           ("host_ms", float("nan")), ("host_ms", -1)):
            report = self.volley_report()
            report["release_samples"][0][key] = value
            self.assertIn("Invalid or incomplete turbo volley release samples", evaluate(report, None))
        report = self.volley_report()
        report["release_samples"] = [report["release_samples"][0]] * 4
        self.assertIn("Fewer than three distinct timed turbo volley identities", evaluate(report, None))
        report = self.volley_report()
        report["release_samples"] = report["release_samples"][:2]
        self.assertTrue(evaluate(report, None))
        report["release_samples"] = [None, [], 3]
        self.assertTrue(evaluate(report, None))

    def test_character_and_volley_flags_validate_before_launch(self):
        for character in ("-1", "8", "1.0", "invalid"):
            with self.assertRaises(SystemExit), redirect_stderr(StringIO()):
                main(["--character", character])
        for workload in ("throw", "rapid-five", "speed-rapid-five", "shove"):
            with self.assertRaises(SystemExit), redirect_stderr(StringIO()):
                main(["--workload", workload, "--require-volleys"])
            report = self.volley_report()
            report["workload"] = workload
            self.assertTrue(evaluate(report, None))
        for value in (None, -1, 8, False, True, "3"):
            report = self.volley_report()
            report["character"] = value
            self.assertTrue(evaluate(report, None))
        for value in (None, 0, 1, "true"):
            report = self.volley_report()
            report["require_volleys"] = value
            self.assertTrue(evaluate(report, None))

    def test_launch_forwards_character_and_optional_volley_requirement(self):
        for required in (False, True):
            args = SimpleNamespace(seconds=25, players=2, workload="turbo-B", element="none",
                                   character=3, require_volleys=required,
                                   internet=False, assets=None, data=None)
            with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
                output = Path(directory)
                with patch("netplay_latency.subprocess.Popen") as create:
                    create.return_value.poll.return_value = 0
                    launch(stack, output / "probe", "guest", output / "invitation", output,
                           args, 60, "on")
                    command = create.call_args.args[0]
                    self.assertEqual(command[command.index("--character") + 1], "3")
                    self.assertEqual("--require-volleys" in command, required)

    def test_pair_reports_must_match_character_and_volley_requirement(self):
        for role in ("host", "guest"):
            for field, value in (("character", 2), ("require_volleys", False)):
                guest = self.volley_report()
                host = dict(guest, role="host")
                (host if role == "host" else guest)[field] = value
                with tempfile.TemporaryDirectory() as directory, redirect_stderr(StringIO()) as error, \
                        redirect_stdout(StringIO()), patch("netplay_latency.pathlib.Path.is_file", return_value=True), \
                        patch("netplay_latency.launch", return_value=(object(), Path(directory) / "unused")), \
                        patch("netplay_latency.wait_invitation"), \
                        patch("netplay_latency.read_report", side_effect=[guest, host]):
                    result = main(["--workload", "turbo-B", "--character", "3", "--require-volleys",
                                   "--prediction", "on", "--seconds", "25", "--build-dir", directory])
                    self.assertEqual(result, 1)
                    self.assertIn("loadout differs from requested configuration", error.getvalue())


if __name__ == "__main__":
    unittest.main()
