"""Inspect N64 Legends music routing and prepare a lossless MusyX reader view.

The source bank remains native. The optional Amuse view removes only each
SDIR record's zero runtime pointer; it does not convert the codec or sequences.
This is a research/export helper, not a claim that a soundtrack has been rendered.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

import n64_legends


PROGRAM_SHA256 = "0b60632eb50c7e77957a5c28106b4f353ba69dccbe0b37f1f68d37a36fd6e585"
PROGRAM_BASE = 0xE0000400


def amuse_sample_directory(directory: bytes, samples: bytes) -> bytes:
    """Adapt 28-byte GDL rows to Amuse's older 24-byte relative-project mode.

    GDL e0088834 advances 28 bytes; e0088a60 uses +8 as a runtime pointer.
    Amuse's N64 reader supports both layouts but couples them to its PROJ mode.
    Discarding verified zero runtime fields keeps all sample IDs, ROM offsets,
    pitch/rate, lengths, loop points and the encoded SAMP bytes unchanged.
    """
    n64_legends.sample_directory(directory, samples)
    return b"".join(directory[at:at + 8] + directory[at + 12:at + 28]
                    for at in range(0, len(directory) - 4, 28)) + b"\xff" * 4


def _string(program: bytes, address: int) -> str:
    at = address - PROGRAM_BASE
    n64_legends.checked_end(at, 1, len(program), "native music routing string")
    end = program.find(b"\0", at, min(at + 128, len(program)))
    if end < 0:
        raise ValueError("unterminated native music routing string")
    value = program[at:end].decode("ascii")
    if not value or any(c in value for c in "/\\:") or value in (".", ".."):
        raise ValueError("invalid native music routing component")
    return value


def _read_level_routes(program: bytes) -> list[dict]:
    """Decode the tables verified at US LoadLevel e006ab40 / PlaySong e000cf28.

    Caller verifies the program revision before using these target addresses.
    The level table supplies directory suffix and signed song token at +12/+20.
    Realm rows supply directory prefix at +8. Terminal realm ID is 16.
    """
    realms = {}
    at = 0xE006C360 - PROGRAM_BASE
    for _ in range(32):
        n64_legends.checked_end(at, 24, len(program), "native realm music table")
        identity, _, name = struct.unpack_from(">3I", program, at)
        if identity == 16:
            break
        if identity in realms:
            raise ValueError("duplicate native realm ID")
        realms[identity] = _string(program, name)
        at += 24
    else:
        raise ValueError("unterminated native realm music table")
    routes = []
    at = 0xE006C468 - PROGRAM_BASE
    for _ in range(128):
        n64_legends.checked_end(at, 40, len(program), "native level music table")
        realm, stage = struct.unpack_from(">2I", program, at)
        if realm == 16:
            return routes
        if realm not in realms:
            raise ValueError("unknown native level realm")
        suffix, = struct.unpack_from(">I", program, at + 12)
        token, = struct.unpack_from(">H", program, at + 20)
        song = token & 0x3FF
        row = {"level": realms[realm] + _string(program, suffix),
               "realm_id": realm, "stage_id": stage, "native_song_token": token,
               "song_id": None, "group_id": None, "song": None}
        if token != 0xFFFF:
            if song >= 39:
                raise ValueError("native level song ID is out of range")
            pointer, = struct.unpack_from(">I", program, 0xE000EE1C - PROGRAM_BASE + song * 4)
            group, = struct.unpack_from(">H", program, 0xE00A8A82 - PROGRAM_BASE + song * 4)
            row.update(song_id=song, group_id=group, song=_string(program, pointer))
        routes.append(row)
        at += 40
    raise ValueError("unterminated native level music table")


def level_routes(program: bytes) -> list[dict]:
    if hashlib.sha256(program).hexdigest() != PROGRAM_SHA256:
        raise ValueError("music routing requires the verified US Legends game.bin")
    return _read_level_routes(program)


def _write_new_or_identical(path: Path, data: bytes) -> None:
    if path.is_symlink():
        raise ValueError(f"refusing symbolic output: {path}")
    if path.exists():
        if path.read_bytes() != data:
            raise FileExistsError(f"refusing to overwrite different file: {path}")
        return
    with path.open("xb") as stream:
        stream.write(data)


def prepare(source: Path, output: Path, *, amuse_view: bool = False) -> dict:
    source = source.resolve(strict=True)
    if output.is_symlink():
        raise ValueError("output cannot be a symbolic link")
    output = output.resolve()
    if output == source or source in output.parents:
        raise ValueError("research output must be outside the extracted native tree")
    program = (source / "_program/game.bin").read_bytes()
    routes = level_routes(program)
    audio = source / "audio"
    payloads = {f"gauntlet.{ext}": (audio / f"gauntlet.{ext}").read_bytes()
                for ext in ("proj", "pool", "sdir", "samp")}
    groups = n64_legends.music_groups(payloads["gauntlet.proj"])
    directory = amuse_sample_directory(payloads["gauntlet.sdir"], payloads["gauntlet.samp"])
    group_ids = {row["id"] for row in groups if row["kind"] == "song"}
    paths = {p.name.casefold(): p for p in audio.glob("*.song")}
    for row in routes:
        row["map_present"] = (source / "levels" / row["level"] / "worlds.wad").is_file()
        if row["song"] is None:
            continue
        path = paths.get(row["song"].casefold())
        if path is None or row["group_id"] not in group_ids:
            raise ValueError(f"{row['level']}: missing native song or group")
        n64_legends.song_header(path.read_bytes())
        row["song"] = path.name
    manifest = {"format": "gauntlet-legends-n64-music-routing", "version": 1,
                "program_sha256": PROGRAM_SHA256, "routes": routes,
                "sample_count": (len(directory) - 4) // 24,
                "amuse_reader_view": amuse_view,
                "rendered_audio": [],
                "notes": ["Native song data needs a MusyX sequencer; it is not PCM audio.",
                          "Amuse view changes SDIR layout only, not its sample codec.",
                          "Tower routing can be overridden by progression movies."]}
    planned = {"music-routing.json": (json.dumps(manifest, indent=2) + "\n").encode("utf-8")}
    if amuse_view:
        planned.update(payloads)
        planned["gauntlet.sdir"] = directory
        planned.update({path.name: path.read_bytes() for path in paths.values()})
    # Check every collision before writing anything.
    output.mkdir(parents=True, exist_ok=True)
    for name, data in planned.items():
        path = output / name
        if path.is_symlink() or (path.exists() and path.read_bytes() != data):
            raise FileExistsError(f"refusing different existing output: {path}")
    for name, data in planned.items():
        _write_new_or_identical(output / name, data)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="native extracted N64 root")
    parser.add_argument("output", type=Path, help="new research output directory, outside native tree")
    parser.add_argument("--amuse-view", action="store_true", help="also prepare a compatible SDIR reader view")
    args = parser.parse_args()
    try:
        result = prepare(args.source, args.output, amuse_view=args.amuse_view)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Verified {len(result['routes'])} level routes and {result['sample_count']} native samples")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
