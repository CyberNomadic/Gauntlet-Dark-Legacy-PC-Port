import copy
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import netplay_scene


class PlayableSceneTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="netplay scene ")
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)

    def test_digest_uses_contents_paths_and_case_independent_order(self):
        one, two = self.root / "one", self.root / "two"
        one.mkdir()
        two.mkdir()
        (one / "B.WAD").write_bytes(b"second")
        (one / "a.ngc").write_bytes(b"first")
        (two / "A.NGC").write_bytes(b"first")
        (two / "b.wad").write_bytes(b"second")
        original = netplay_scene.asset_digest(one)
        self.assertEqual(original, netplay_scene.asset_digest(two))
        (two / "A.NGC").write_bytes(b"other")
        self.assertNotEqual(original, netplay_scene.asset_digest(two))
        (two / "A.NGC").write_bytes(b"first")
        (two / "b.wad").rename(two / "c.wad")
        self.assertNotEqual(original, netplay_scene.asset_digest(two))

    def test_missing_or_empty_asset_tree_cannot_match(self):
        for path in (self.root, self.root / "missing"):
            with self.assertRaisesRegex(ValueError, "No assets"):
                netplay_scene.asset_digest(path)

    def test_unreadable_asset_directory_does_not_produce_a_partial_identity(self):
        def broken_walk(root, *, followlinks, onerror):
            onerror(PermissionError("cannot enumerate native assets"))
            return iter(())

        with mock.patch.object(netplay_scene.os, "walk", side_effect=broken_walk):
            with self.assertRaisesRegex(PermissionError, "cannot enumerate"):
                netplay_scene.asset_digest(self.root)

    def test_test_characters_never_retain_disk_save_authority(self):
        original = {"level": "G1", "party": [{"player": 3, "slot": 2, "color": "BLU"}]}
        saved = copy.deepcopy(original)
        host = netplay_scene.test_scenario(original, False)
        guest = netplay_scene.test_scenario(original, True)
        self.assertEqual(original, saved)
        for selection in (host, guest):
            self.assertEqual(selection["party"][0]["player"], 0)
            self.assertNotIn("slot", selection["party"][0])
        self.assertNotEqual(host["party"][0]["color"], guest["party"][0]["color"])
        for invalid in ({}, {"party": []}, {"party": [{}, {}]},
                        {"screen": "ending", "party": [{}]}):
            with self.assertRaises(ValueError):
                netplay_scene.test_scenario(invalid, False)

    def test_default_scenario_carries_valid_native_potion_types(self):
        source = json.loads((netplay_scene.ROOT / "tests/scenarios/netplay-g1.json").read_text())
        for guest in (False, True):
            selection = netplay_scene.test_scenario(source, guest)
            player = selection["party"][0]
            self.assertEqual(player["level"], 60)
            self.assertEqual(len(player["potions"]), 9)
            self.assertTrue(all(1 <= kind <= 4 for kind in player["potions"]))

    def test_two_windows_have_safe_isolated_display_and_device_defaults(self):
        defaults = json.loads((netplay_scene.ROOT / "data/config.json").read_text(encoding="utf-8"))
        original = copy.deepcopy(defaults)
        for guest in (False, True):
            config = netplay_scene.test_config(defaults, guest, 30)
            self.assertTrue(config["display"]["vsync"])
            self.assertEqual(config["display"]["windowMode"], "windowed")
            self.assertEqual(config["timing"]["tickRate"], 60)
            self.assertEqual(config["timing"]["gameplayFrameRate"], 30)
            self.assertEqual(config["display"]["maxFrameRate"], 30)
            self.assertEqual(config["audio"], defaults["audio"])
            self.assertEqual(config["controls"]["players"][0]["device"], "keyboard" if guest else "")
            self.assertTrue(all(p["device"] == "none" for p in config["controls"]["players"][1:]))
        self.assertEqual(defaults, original)

    def test_mixed_video_changes_only_guest_presentation_not_session_timing(self):
        defaults = json.loads((netplay_scene.ROOT / "data/config.json").read_text(encoding="utf-8"))
        host = netplay_scene.test_config(defaults, False, 30, True)
        guest = netplay_scene.test_config(defaults, True, 30, True)
        self.assertEqual(host, netplay_scene.test_config(defaults, False, 30))
        self.assertEqual(host["timing"]["tickRate"], 60)
        self.assertEqual(guest["timing"]["tickRate"], 60)
        self.assertEqual(host["timing"]["gameplayFrameRate"], 30)
        self.assertEqual(guest["timing"]["gameplayFrameRate"], 60)
        self.assertEqual(guest["display"]["maxFrameRate"], 60)
        self.assertEqual(guest["display"]["sampleCount"], 4)
        self.assertEqual(guest["display"]["windowWidth"], 960)
        self.assertEqual(guest["display"]["windowHeight"], 540)
        self.assertEqual(guest["camera"]["horizontalFovDegrees"], 80)
        for effect in ("bloom", "ambientOcclusion", "depthOfField"):
            self.assertTrue(guest["display"][effect])
            self.assertFalse(host["display"][effect])

    def test_requested_frame_cap_controls_gameplay_and_menu_not_simulation(self):
        defaults = json.loads((netplay_scene.ROOT / "data/config.json").read_text(encoding="utf-8"))
        for fps in (30, 60):
            for guest in (False, True):
                with self.subTest(fps=fps, guest=guest):
                    config = netplay_scene.test_config(defaults, guest, fps)
                    self.assertEqual(config["timing"]["tickRate"], 60)
                    self.assertEqual(config["timing"]["gameplayFrameRate"], fps)
                    self.assertEqual(config["display"]["maxFrameRate"], fps)

    def test_runtime_errors_and_crashes_are_not_successful_visual_checks(self):
        log = self.root / "game.log"
        child = mock.Mock()
        child.poll.return_value = None
        log.write_text("still loading", encoding="utf-8")
        self.assertFalse(netplay_scene.playing(netplay_scene.healthy(child, log)))
        for line in ("ERROR ASSERTION FAILED", "Connection failed (3)", "Online scene stopped (1)"):
            log.write_text(line, encoding="utf-8")
            with self.assertRaises(RuntimeError):
                netplay_scene.healthy(child, log)
        log.write_text("Native online stage loaded: L1 (epoch 2, peer 1)\n"
                       "Native online progress: epoch 2 tick 300 (host)\n", encoding="utf-8")
        child.poll.return_value = 1
        with self.assertRaises(RuntimeError):
            netplay_scene.healthy(child, log)

    def test_expected_level_requires_its_own_completed_loading_barrier(self):
        first = ("Native online stage loaded: L1 (epoch 2, peer 1)\n"
                 "Native online progress: epoch 2 tick 300 (host)\n")
        self.assertTrue(netplay_scene.playing(first))
        self.assertTrue(netplay_scene.playing(first, "L1"))
        self.assertFalse(netplay_scene.playing(first, "G1"))
        pending = first + "Native online stage loaded: G1 (epoch 3, peer 1)\n"
        self.assertFalse(netplay_scene.playing(pending, "G1"))
        started = pending + "Native online progress: epoch 3 tick 0 (host)\n"
        self.assertFalse(netplay_scene.playing(started, "G1"))
        final = started + "Native online progress: epoch 3 tick 1 (host)\n"
        self.assertTrue(netplay_scene.playing(final, "G1"))
        self.assertFalse(netplay_scene.playing(final, "L1"))

    def test_smoke_test_requires_completed_native_ticks_in_the_loaded_stage(self):
        first = ("Native online stage loaded: L1 (epoch 2, peer 1)\n"
                 "Native online progress: epoch 2 tick 600 (host)\n")
        self.assertEqual(netplay_scene.progress(first), (2, 600))
        pending = first + "Native online stage loaded: G1 (epoch 3, peer 1)\n"
        self.assertIsNone(netplay_scene.progress(pending))
        self.assertIsNone(netplay_scene.progress(
            pending + "Native online progress: epoch 2 tick 900 (host)\n"))
        started = pending + "Native online progress: epoch 3 tick 0 (host)\n"
        self.assertEqual(netplay_scene.progress(started), (3, 0))
        final = started + "Native online progress: epoch 3 tick 300 (host)\n"
        self.assertEqual(netplay_scene.progress(final), (3, 300))
        self.assertTrue(netplay_scene.advancing((3, 0), netplay_scene.progress(final)))
        for original, latest in ((None, (3, 300)), ((3, 0), None), ((3, 300), (3, 300)),
                                 ((3, 300), (2, 600)), ((3, 300), (3, 100)),
                                 ((3, 300), (4, 0))):
            self.assertFalse(netplay_scene.advancing(original, latest))

    def test_old_replica_progress_does_not_satisfy_native_admission(self):
        old = ("Online stage loaded: G1 (epoch 3, host)\n"
               "Online scene test - Start pauses\n"
               "Online checkpoint progress: epoch 3 tick 600 (host)\n")
        self.assertIsNone(netplay_scene.stage(old))
        self.assertIsNone(netplay_scene.progress(old))
        self.assertFalse(netplay_scene.playing(old))
        self.assertFalse(netplay_scene.playing(
            "Native online stage loaded: G1 (epoch 3, peer 1)\n" + old))

    def test_startup_requires_both_native_peers_in_the_same_loaded_epoch(self):
        host = ("Native online stage loaded: G1 (epoch 3, peer 1)\n"
                "Native online progress: epoch 3 tick 300 (host)\n")
        guest = ("Native online stage loaded: G1 (epoch 3, peer 2)\n"
                 "Native online progress: epoch 3 tick 297 (guest)\n")
        self.assertTrue(netplay_scene.ready_pair([host, guest], "G1"))
        self.assertFalse(netplay_scene.ready_pair([host], "G1"))
        self.assertFalse(netplay_scene.ready_pair([host, guest], "L1"))
        for changed in (guest.replace("G1", "L1"), guest.replace("epoch 3", "epoch 4"),
                        guest.replace("tick 297", "tick 0")):
            self.assertFalse(netplay_scene.ready_pair([host, changed]))
        for changed in (guest.replace("epoch 3", "epoch 0"), ""):
            self.assertIsNone(netplay_scene.stage(changed))
            self.assertFalse(netplay_scene.ready_pair([host, changed]))


if __name__ == "__main__":
    unittest.main()
