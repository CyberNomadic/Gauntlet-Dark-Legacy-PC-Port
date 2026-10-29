"""Native MusyX routing and reader-view tests without an external synthesizer."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts/assets"))
import n64_legends_music as music


def synthetic_program():
    result = bytearray(0xB0000)
    base = music.PROGRAM_BASE
    for at, value in ((0xAC000, b"mount\0"), (0xAC010, b"1\0"), (0xAC020, b"mount1.song\0")):
        result[at:at + len(value)] = value
    struct.pack_into(">6I", result, 0xE006C360 - base, 3, 0, base + 0xAC000, 0, 0, 0)
    struct.pack_into(">I", result, 0xE006C378 - base, 16)
    at = 0xE006C468 - base
    struct.pack_into(">4I", result, at, 3, 0, 0, base + 0xAC010)
    struct.pack_into(">H", result, at + 20, 0x2000)
    struct.pack_into(">I", result, at + 40, 16)
    struct.pack_into(">I", result, 0xE000EE1C - base, base + 0xAC020)
    return result


class N64LegendsMusicTests(unittest.TestCase):
    def test_sdir_view_changes_only_zero_runtime_pointer(self):
        record = struct.pack(">HH6I", 1, 0, 0, 0, 0x3C002B11, 0x03000040, 2, 60)
        data = record + b"\xff" * 4
        expected = record[:8] + record[12:] + b"\xff" * 4
        self.assertEqual(music.amuse_sample_directory(data, bytes(296)), expected)

    def test_sdir_view_rejects_runtime_state_and_truncation(self):
        record = bytearray(struct.pack(">HH6I", 1, 0, 0, 0, 0x3C002B11, 0x03000040, 2, 60))
        for offset in (2, 8):
            broken = record.copy()
            broken[offset] = 1
            with self.assertRaises(ValueError):
                music.amuse_sample_directory(broken + b"\xff" * 4, bytes(296))
        with self.assertRaises(ValueError):
            music.amuse_sample_directory(record + b"\xff" * 4, bytes(295))
        with self.assertRaises(ValueError):
            music.amuse_sample_directory(record, bytes(296))

    def test_native_routes_use_table_tokens_and_no_music_sentinel(self):
        data = synthetic_program()
        self.assertEqual(music._read_level_routes(data), [
            {"level": "mount1", "realm_id": 3, "stage_id": 0, "native_song_token": 0x2000,
             "song_id": 0, "group_id": 0, "song": "mount1.song"}])
        struct.pack_into(">H", data, 0xE006C468 - music.PROGRAM_BASE + 20, 0xFFFF)
        self.assertIsNone(music._read_level_routes(data)[0]["song"])

    def test_revision_and_bounds_are_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "verified US"):
            music.level_routes(synthetic_program())
        with self.assertRaises(ValueError):
            music._read_level_routes(b"\0" * 80)
        data = synthetic_program()
        struct.pack_into(">H", data, 0xE006C468 - music.PROGRAM_BASE + 20, 0x2027)
        with self.assertRaisesRegex(ValueError, "out of range"):
            music._read_level_routes(data)
        data = synthetic_program()
        struct.pack_into(">I", data, 0xE000EE1C - music.PROGRAM_BASE, 0xFFFFFFFF)
        with self.assertRaises(ValueError):
            music._read_level_routes(data)

    def test_output_preserves_different_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "out"
            music._write_new_or_identical(path, b"native")
            music._write_new_or_identical(path, b"native")
            with self.assertRaises(FileExistsError):
                music._write_new_or_identical(path, b"different")
            self.assertEqual(path.read_bytes(), b"native")

    @unittest.skipUnless((Path(__file__).resolve().parents[2] / "assets/n64/extracted/_program/game.bin").exists(),
                         "native N64 corpus is optional")
    def test_native_corpus_routes_and_preserving_export(self):
        source = Path(__file__).resolve().parents[2] / "assets/n64/extracted"
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "music"
            result = music.prepare(source, output, amuse_view=True)
            self.assertEqual(result["sample_count"], 240)
            routes = {r["level"]: r for r in result["routes"]}
            self.assertEqual(len(routes), 37)
            self.assertEqual(routes["ice2"]["song"], "Dungeon.song")
            self.assertEqual(routes["town4"]["song"], "Townt4.song")
            self.assertIsNone(routes["secret1"]["song"])
            self.assertEqual(music.prepare(source, output, amuse_view=True), result)
            self.assertEqual((output / "gauntlet.samp").read_bytes(),
                             (source / "audio/gauntlet.samp").read_bytes())
            with self.assertRaises(FileExistsError):
                music.prepare(source, output, amuse_view=False)


if __name__ == "__main__":
    unittest.main()
