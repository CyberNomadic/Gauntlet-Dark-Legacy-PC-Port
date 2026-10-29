"""Export extracted Dreamcast Legends STR music to PCM WAV without changing native files.

FFmpeg's dcstr demuxer and adpcm_aica decoder handle these Sega streams directly.
This optional inspection tool is not a dependency of the game or its native assets.
The source header records total ADPCM bytes, including both channels, not PCM frames.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import wave


@dataclass(frozen=True)
class StreamInfo:
    sample_rate: int
    channels: int
    frames: int
    bits_per_sample: int
    interleave: int
    blocks: int


def read_stream_info(path: Path) -> StreamInfo:
    """Validate the one-track Sega Stream Asset Builder layout used by this disc."""
    with path.open("rb") as source:
        header = source.read(0x800)
    if len(header) != 0x800 or not header[0xD4:].startswith(b"\0Sega Stream"):
        raise ValueError(f"{path.name}: not a Sega STR stream")
    tracks, rate, bits, interleave, blocks, data_size, channels = struct.unpack_from("<7I", header)
    if tracks != 1 or channels not in (1, 2) or bits != 4:
        raise ValueError(f"{path.name}: unsupported STR channel/codec layout")
    if not 8000 <= rate <= 192000 or not interleave or interleave % 0x800:
        raise ValueError(f"{path.name}: invalid STR rate/interleave")
    capacity = blocks * channels * interleave
    if capacity != path.stat().st_size - 0x800 or not 0 < data_size <= capacity:
        raise ValueError(f"{path.name}: invalid STR payload size")
    if data_size * 2 % channels:
        raise ValueError(f"{path.name}: incomplete STR sample frame")
    return StreamInfo(rate, channels, data_size * 2 // channels, bits, interleave, blocks)


def digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            checksum.update(chunk)
    return checksum.hexdigest()


def verify_wave(path: Path, info: StreamInfo) -> None:
    with wave.open(str(path), "rb") as source:
        if (source.getnchannels(), source.getframerate(), source.getsampwidth(), source.getnframes()) != (
                info.channels, info.sample_rate, 2, info.frames):
            raise ValueError(f"{path.name}: decoded WAV does not match native STR sample count/rate")
        # Check the physical payload too; a truncated file can retain a valid RIFF header.
        frames = 0
        while chunk := source.readframes(65536):
            if len(chunk) % (info.channels * 2):
                raise ValueError(f"{path.name}: incomplete PCM frame")
            frames += len(chunk) // (info.channels * 2)
        if frames != info.frames:
            raise ValueError(f"{path.name}: truncated PCM payload")


def decode_stream(source: Path, target: Path, ffmpeg: str, info: StreamInfo) -> str:
    if target.is_symlink():
        raise ValueError(f"Refusing symbolic output: {target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix=".legends-audio-", suffix=".wav",
                                     dir=target.parent, delete=False) as handle:
        temporary = Path(handle.name)
    try:
        completed = subprocess.run(
            [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", str(source),
             "-map", "0:a:0", "-map_metadata", "-1", "-af", f"atrim=end_sample={info.frames}",
             "-c:a", "pcm_s16le", "-fflags", "+bitexact", "-flags:a", "+bitexact", str(temporary)],
            capture_output=True, text=True, check=False)
        if completed.returncode:
            raise ValueError(f"{source.name}: FFmpeg failed: {completed.stderr.strip()}")
        verify_wave(temporary, info)
        checksum = digest(temporary)
        if target.exists():
            if digest(target) != checksum:
                raise ValueError(f"Refusing to overwrite different output: {target}")
        else:
            # Exclusive creation prevents replacing a file another process creates meanwhile.
            with target.open("xb") as destination, temporary.open("rb") as decoded:
                shutil.copyfileobj(decoded, destination)
        return checksum
    finally:
        temporary.unlink(missing_ok=True)


def _text(data: bytes, offset: int, size: int) -> str:
    value = data[offset:offset + size]
    if len(value) != size:
        raise ValueError("Truncated native text field")
    return value.split(b"\0", 1)[0].decode("ascii")


def level_references(path: Path, stream_names: list[str]) -> list[dict]:
    """Read DC's 12-byte WAD directory, 60-byte AUDS and 144-byte LEVL records.

    Matching files expose the native stream base and suffix variants; they do not claim
    that every retained/disabled WDATA record is a playable level on this disc.
    """
    data = path.read_bytes()
    if len(data) < 8:
        raise ValueError(f"{path.name}: truncated WAD")
    directory, count = struct.unpack_from("<II", data)
    if count > 4096 or directory < 8 or directory + count * 12 > len(data):
        raise ValueError(f"{path.name}: invalid WAD directory")
    sections = {}
    for index in range(count):
        raw_tag, offset, size = struct.unpack_from("<4sII", data, directory + index * 12)
        tag = raw_tag[::-1].decode("ascii")
        if tag in sections or not 8 <= offset <= directory:
            raise ValueError(f"{path.name}: invalid WAD section")
        sections[tag] = (offset, size)

    def records(tag: str, stride: int):
        if tag not in sections:
            return []
        offset, count = sections[tag]
        end = min([directory] + [at for at, _ in sections.values() if at > offset])
        if count > (end - offset) // stride:
            raise ValueError(f"{path.name}: invalid {tag} record count")
        return [offset + i * stride for i in range(count)]

    audio = records("AUDS", 60)
    result = []
    for level in records("LEVL", 144):
        # DC omits the later GameCube 16-byte movie field; +90 is a runtime pointer.
        audio_index = struct.unpack_from("<h", data, level + 74)[0]
        if audio_index < 0 or audio_index >= len(audio):
            raise ValueError(f"{path.name}: invalid level audio index")
        at = audio[audio_index]
        base = _text(data, at + 24, 16)
        matches = []
        for name in stream_names:
            stem = Path(name).stem.lower()
            suffix = stem[len(base):] if base and stem.startswith(base.lower()) else None
            if suffix is not None and (suffix == "" or len(suffix) == 1 and "a" <= suffix <= "z"):
                matches.append(name)
        result.append(dict(worlddata=path.name, level=_text(data, level + 8, 4),
                           title=_text(data, level + 20, 16),
                           enabled=struct.unpack_from("<h", data, level + 4)[0],
                           audio_index=audio_index, native_bank=_text(data, at, 16),
                           native_stream_base=base,
                           area_count=struct.unpack_from("<h", data, at + 40)[0],
                           matching_stream_files=matches))
    return result


def write_manifest(path: Path, value: dict) -> None:
    payload = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    if path.is_symlink():
        raise ValueError(f"Refusing symbolic manifest: {path}")
    if path.exists():
        if path.read_bytes() != payload:
            raise ValueError(f"Refusing to overwrite different manifest: {path}")
        return
    with path.open("xb") as destination:
        destination.write(payload)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("streams", type=Path, help="extracted disc AUDIO/STREAMS directory")
    parser.add_argument("output", type=Path, nargs="?", help="separate directory for PCM WAV files")
    parser.add_argument("--worlddata", type=Path, help="optional extracted native/worlddata directory")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="FFmpeg executable; requires dcstr/adpcm_aica")
    parser.add_argument("--list", action="store_true", help="inspect stream metadata without decoding")
    args = parser.parse_args(argv)
    if not args.streams.is_dir():
        parser.error("streams must be an existing directory")
    if not args.list and args.output is None:
        parser.error("an output directory is required unless --list is used")
    if args.output is not None and args.output.resolve() == args.streams.resolve():
        parser.error("output must be separate from the native stream directory")
    streams = sorted(p for p in args.streams.iterdir() if p.is_file() and p.suffix.lower() == ".str")
    if not streams:
        parser.error("no STR streams found")
    try:
        rows = []
        for source in streams:
            info = read_stream_info(source)
            row = dict(native_file=source.name, source_sha256=digest(source), **asdict(info),
                       seconds=info.frames / info.sample_rate)
            if not args.list:
                target = args.output / (source.stem + ".wav")
                row.update(wav_file=target.name, wav_sha256=decode_stream(source, target, args.ffmpeg, info))
            rows.append(row)
            print(f"{source.name}: {info.sample_rate} Hz, {info.channels} channel(s), "
                  f"{info.frames} frames, {info.frames / info.sample_rate:.3f}s", flush=True)
        references = []
        if args.worlddata:
            if not args.worlddata.is_dir():
                raise ValueError("worlddata directory does not exist")
            for source in sorted(args.worlddata.glob("*.wad")):
                references.extend(level_references(source, [p.name for p in streams]))
        if not args.list:
            write_manifest(args.output / "manifest.json",
                           dict(format="dreamcast-legends-soundtrack-v1", streams=rows,
                                level_references=references))
        print(f"{'Inspected' if args.list else 'Exported and verified'} {len(rows)} native streams")
        return 0
    except (OSError, ValueError, UnicodeError, struct.error, wave.Error) as error:
        parser.exit(1, f"Audio export failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
