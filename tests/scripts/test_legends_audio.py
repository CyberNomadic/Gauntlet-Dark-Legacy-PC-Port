"""Synthetic Sega STR metadata/export tests; no game or external decoder required."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "assets"))
import legends_audio as audio


def stream_bytes(channels=2):
    result = bytearray(0x800 + channels * 0x2000)
    struct.pack_into("<7I", result, 0, 1, 44100, 4, 0x2000, 1, 8, channels)
    result[0xD4:0xE0] = b"\0Sega Stream"
    return result


def write_wave(path, info):
    with wave.open(str(path), "wb") as output:
        output.setnchannels(info.channels)
        output.setframerate(info.sample_rate)
        output.setsampwidth(2)
        output.writeframes(bytes(info.frames * info.channels * 2))


class LegendsAudioTests(unittest.TestCase):
    def test_str_metadata_preserves_native_frame_count(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "TEST.STR"
            path.write_bytes(stream_bytes())
            self.assertEqual(audio.read_stream_info(path), audio.StreamInfo(44100, 2, 8, 4, 0x2000, 1))
            path.write_bytes(stream_bytes(1))
            self.assertEqual(audio.read_stream_info(path).frames, 16)

    def test_reject_bad_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "TEST.STR"
            for offset, value in ((0, 0), (4, 0), (8, 16), (12, 12), (16, 2), (20, 0), (24, 4)):
                with self.subTest(offset=offset):
                    data = stream_bytes()
                    struct.pack_into("<I", data, offset, value)
                    path.write_bytes(data)
                    with self.assertRaises(ValueError):
                        audio.read_stream_info(path)
            path.write_bytes(b"not a stream")
            with self.assertRaises(ValueError):
                audio.read_stream_info(path)

    def test_conversion_validates_and_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, target = root / "TEST.STR", root / "TEST.wav"
            source.write_bytes(stream_bytes())
            info = audio.read_stream_info(source)

            def decode(command, **_):
                self.assertIn("atrim=end_sample=8", command)
                write_wave(Path(command[-1]), info)
                return subprocess.CompletedProcess(command, 0, "", "")

            with mock.patch.object(audio.subprocess, "run", side_effect=decode):
                checksum = audio.decode_stream(source, target, "ffmpeg", info)
                self.assertEqual(checksum, audio.digest(target))
                self.assertEqual(checksum, audio.decode_stream(source, target, "ffmpeg", info))
                target.write_bytes(b"user output")
                with self.assertRaisesRegex(ValueError, "overwrite"):
                    audio.decode_stream(source, target, "ffmpeg", info)
                self.assertEqual(target.read_bytes(), b"user output")
            self.assertFalse(list(root.glob(".legends-audio-*")))

    def test_failed_decoder_cannot_create_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, target = root / "TEST.STR", root / "TEST.wav"
            source.write_bytes(stream_bytes())
            with mock.patch.object(audio.subprocess, "run",
                                   return_value=subprocess.CompletedProcess([], 1, "", "bad codec")):
                with self.assertRaisesRegex(ValueError, "bad codec"):
                    audio.decode_stream(source, target, "ffmpeg", audio.read_stream_info(source))
            self.assertFalse(target.exists())
            self.assertFalse(list(root.glob(".legends-audio-*")))

    def test_invalid_wave_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "TEST.wav"
            info = audio.StreamInfo(44100, 2, 8, 4, 0x2000, 1)
            write_wave(path, info)
            audio.verify_wave(path, info)
            with self.assertRaises(ValueError):
                audio.verify_wave(path, audio.StreamInfo(22050, 2, 8, 4, 0x2000, 1))
            path.write_bytes(path.read_bytes()[:-2])
            with self.assertRaises(ValueError):
                audio.verify_wave(path, info)

    def test_manifest_does_not_overwrite_different_file(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "manifest.json"
            audio.write_manifest(target, {"test": 1})
            original = target.read_bytes()
            audio.write_manifest(target, {"test": 1})
            with self.assertRaises(ValueError):
                audio.write_manifest(target, {"test": 2})
            self.assertEqual(target.read_bytes(), original)

    def test_native_level_audio_mapping_retains_missing_streams(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "mount.wad"
            data = bytearray(8 + 60 + 144 + 24)
            struct.pack_into("<II", data, 0, 212, 2)
            data[8:13] = b"MOUNT"
            data[32:38] = b"mount1"
            struct.pack_into("<h", data, 8 + 40, 2)
            data[68 + 8:68 + 10] = b"B1"
            data[68 + 20:68 + 24] = b"Peak"
            struct.pack_into("<4sII", data, 212, b"SDUA", 8, 1)
            struct.pack_into("<4sII", data, 224, b"LVEL", 68, 1)
            path.write_bytes(data)
            result = audio.level_references(path, ["MOUNT1.STR", "MOUNT1A.STR", "MOUNT10.STR"])
            self.assertEqual(result[0]["matching_stream_files"], ["MOUNT1.STR", "MOUNT1A.STR"])
            self.assertEqual(result[0]["level"], "B1")
            self.assertEqual(audio.level_references(path, [])[0]["native_stream_base"], "mount1")
            # The GameCube +90 field is a runtime pointer in DC, not the audio slot.
            struct.pack_into("<h", data, 68 + 90, 123)
            path.write_bytes(data)
            self.assertEqual(audio.level_references(path, [])[0]["audio_index"], 0)
            struct.pack_into("<h", data, 68 + 74, 2)
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                audio.level_references(path, [])


if __name__ == "__main__":
    unittest.main()
