"""Extract native files from a Gauntlet Legends Dreamcast GD-ROM CUE/BIN dump.

The ISO9660 extents are absolute disc LBAs. MODE1 sector headers provide their
addresses, including tracks separated by audio and stored pregaps; concatenating
track files or treating track 3 as a standalone ISO corrupts these extents.
No executable unscrambling or game-data conversion is performed.
"""

from __future__ import annotations

import argparse
from contextlib import ExitStack
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
from typing import BinaryIO

RAW_SECTOR = 2352
SECTOR = 2048
HIGH_DENSITY_LBA = 45000
SYNC = b"\0" + b"\xff" * 10 + b"\0"
RESERVED = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"} | {
    f"{prefix}{n}" for prefix in ("COM", "LPT") for n in range(10)}


def safe_name(name: str) -> str:
    if (not name or name in (".", "..") or name[-1] in " ." or
            any(ord(c) < 32 or c in '/\\:<>"|?*' for c in name) or
            name.split(".")[0].upper() in RESERVED):
        raise ValueError(f"Unsafe disc name: {name!r}")
    return name


def sector_lba(header: bytes) -> int:
    """Decode the BCD minute/second/frame address of one MODE1 raw sector."""
    if len(header) < 16 or header[:12] != SYNC or header[15] != 1:
        raise ValueError("Expected a MODE1/2352 sector header")
    digits = header[12:15]
    # GD-ROM extends the minute tens nibble beyond 9 (the disc exceeds 100 min).
    if any((d & 15) > 9 for d in digits) or any((d >> 4) > 9 for d in digits[1:]):
        raise ValueError("Invalid BCD sector address")
    minute, second, frame = ((d >> 4) * 10 + (d & 15) for d in digits)
    if second >= 60 or frame >= 75:
        raise ValueError("Invalid sector time")
    return (minute * 60 + second) * 75 + frame - 150


@dataclass(frozen=True)
class CueTrack:
    number: int
    path: Path
    mode: str
    index: int


def read_cue(path: Path) -> list[CueTrack]:
    """Read separate-file raw tracks, refusing external paths and other modes."""
    tracks = []
    current_file = None
    number = None
    mode = None
    used_files = set()
    for raw in path.read_text(encoding="utf-8-sig").splitlines():
        line = raw.strip()
        if not line or line.startswith("REM "):
            continue
        match = re.fullmatch(r'FILE "([^"]+)" BINARY', line)
        if match:
            if number is not None:
                raise ValueError("Track is missing INDEX 01")
            name = safe_name(match[1])
            current_file = path.parent / name
            continue
        match = re.fullmatch(r"TRACK (\d+) (AUDIO|MODE1/2352)", line)
        if match:
            if number is not None or current_file is None or current_file in used_files:
                raise ValueError("Expected one complete track per BIN file")
            number, mode = int(match[1]), match[2]
            continue
        match = re.fullmatch(r"INDEX (00|01) (\d+):(\d+):(\d+)", line)
        if match and number is not None:
            minute, second, frame = map(int, match.groups()[1:])
            if second >= 60 or frame >= 75:
                raise ValueError("Invalid CUE time")
            if match[1] == "01":
                tracks.append(CueTrack(number, current_file, mode,
                                       (minute * 60 + second) * 75 + frame))
                used_files.add(current_file)
                number = None
            continue
        raise ValueError(f"Unsupported CUE line: {line}")
    if number is not None or not tracks:
        raise ValueError("Incomplete CUE")
    if len({track.number for track in tracks}) != len(tracks):
        raise ValueError("Duplicate track number")
    return tracks


@dataclass(frozen=True)
class DataTrack:
    source: CueTrack
    first_lba: int
    sectors: int
    stream: BinaryIO


@dataclass(frozen=True)
class DiscFile:
    path: PurePosixPath
    lba: int
    size: int


@dataclass(frozen=True)
class PackedFile:
    path: PurePosixPath
    offset: int
    size: int


def packed_files(directory: bytes, payload_size: int) -> list[PackedFile]:
    """Read SIZES.ROM's path table; offset -1 denotes an external disc file.

    The native consumers at SH4 0x8c0e2260 and 0x8c0e24c0 use 16-byte records:
    size, DISK.ROM byte offset, and four signed string-pool offsets, terminated
    by the first negative component. The pool follows the complete table.
    """
    if len(directory) < 4:
        raise ValueError("Truncated SIZES.ROM header")
    count = struct.unpack_from("<I", directory)[0]
    if not count or count > (len(directory) - 4) // 16:
        raise ValueError("SIZES.ROM table lies outside its file")
    pool = directory[4 + count * 16:]
    files = []
    names = set()
    for index in range(count):
        size, offset, *components = struct.unpack_from("<Ii4h", directory, 4 + index * 16)
        parts = []
        for at in components:
            if at < 0:
                break
            end = pool.find(b"\0", at)
            if at >= len(pool) or end < 0:
                raise ValueError("SIZES.ROM name lies outside its string pool")
            parts.append(safe_name(pool[at:end].decode("ascii")))
        if not parts:
            raise ValueError("SIZES.ROM entry has no name")
        path = PurePosixPath(*parts)
        key = path.as_posix().casefold()
        if key in names:
            raise ValueError("Duplicate SIZES.ROM path")
        names.add(key)
        if offset < -1 or offset >= 0 and (offset > payload_size or size > payload_size - offset):
            raise ValueError(f"SIZES.ROM extent lies outside DISK.ROM: {path}")
        files.append(PackedFile(path, offset, size))
    for file in files:
        if any(parent.as_posix().casefold() in names for parent in file.path.parents
               if parent != PurePosixPath()):
            raise ValueError("SIZES.ROM file/directory path conflict")
    return files


def unpack_native(output: Path) -> dict:
    """Restore named native ROM/WAD payloads byte-for-byte without conversion."""
    output = output.resolve()
    directory = (output / "disc/SIZES.ROM").read_bytes()
    disk = output / "disc/DISK.ROM"
    files = packed_files(directory, disk.stat().st_size)
    rows = []
    external = []
    with disk.open("rb") as stream:
        for file in files:
            if file.offset < 0:
                external.append(dict(path=file.path.as_posix(), declared_size=file.size))
                continue
            target = output / "native" / Path(file.path)
            if not target.resolve().is_relative_to(output):
                raise ValueError("Native output path escapes extraction directory")
            stream.seek(file.offset)

            def chunks():
                left = file.size
                while left:
                    data = stream.read(min(left, 1024 * 1024))
                    if not data:
                        raise ValueError("Truncated DISK.ROM payload")
                    yield data
                    left -= len(data)

            digest = write_verified(target, chunks())
            rows.append(dict(path=file.path.as_posix(), offset=file.offset,
                             size=file.size, sha256=digest))
    result = dict(format="dreamcast-legends-native-v1", files=rows, external=external,
                  sizes_sha256=hashlib.sha256(directory).hexdigest(),
                  scope="Exact DISK.ROM payloads; external records may be absent from the disc")
    write_verified(output / "native-manifest.json",
                   [(json.dumps(result, indent=2) + "\n").encode()])
    return result


class Disc:
    """Bounded reader for the high-density ISO9660 filesystem."""

    def __init__(self, cue: Path):
        self.cue = cue
        self._files = ExitStack()
        self.tracks = []
        try:
            for track in read_cue(cue):
                if track.mode != "MODE1/2352":
                    continue
                stream = self._files.enter_context(track.path.open("rb"))
                length = track.path.stat().st_size
                if length % RAW_SECTOR or track.index >= length // RAW_SECTOR:
                    raise ValueError("Truncated raw track or INDEX beyond its end")
                stream.seek(track.index * RAW_SECTOR)
                first = sector_lba(stream.read(16))
                self.tracks.append(DataTrack(track, first,
                                             length // RAW_SECTOR - track.index, stream))
            self.tracks.sort(key=lambda track: track.first_lba)
            for a, b in zip(self.tracks, self.tracks[1:]):
                if a.first_lba + a.sectors > b.first_lba:
                    raise ValueError("Overlapping data tracks")
            self.boot = self.read(HIGH_DENSITY_LBA, 16 * SECTOR)
            if self.boot[:16] != b"SEGA SEGAKATANA " or self.boot[128:144] != b"GAUNTLET LEGENDS":
                raise ValueError("Expected a Dreamcast Gauntlet Legends GD-ROM")
            pvd = self.read(HIGH_DENSITY_LBA + 16, SECTOR)
            if pvd[:7] != b"\x01CD001\x01" or self._both(pvd, 128, 2) != SECTOR:
                raise ValueError("Unsupported ISO9660 volume descriptor")
            self.volume = pvd[40:72].decode("ascii").strip()
            self.entries = []
            root = self._record(pvd, 156)
            if not root[2] & 2:
                raise ValueError("ISO root is not a directory")
            self._walk(root[0], root[1], PurePosixPath(), set(), set(), 0)
        except BaseException:
            self.close()
            raise

    def close(self):
        self._files.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    @staticmethod
    def _both(data: bytes, at: int, width: int) -> int:
        a = int.from_bytes(data[at:at + width], "little")
        b = int.from_bytes(data[at + width:at + width * 2], "big")
        if a != b:
            raise ValueError("Mismatched ISO9660 little/big-endian fields")
        return a

    def _track(self, lba: int) -> DataTrack:
        for track in self.tracks:
            if track.first_lba <= lba < track.first_lba + track.sectors:
                return track
        raise ValueError(f"ISO extent points outside data tracks at LBA {lba}")

    def check_extent(self, lba: int, size: int):
        if size < 0 or lba < 0:
            raise ValueError("Negative extent")
        sectors = (size + SECTOR - 1) // SECTOR
        while sectors:
            track = self._track(lba)
            take = min(sectors, track.first_lba + track.sectors - lba)
            sectors -= take
            lba += take

    def chunks(self, lba: int, size: int):
        self.check_extent(lba, size)
        while size:
            track = self._track(lba)
            count = min((size + SECTOR - 1) // SECTOR, 512,
                        track.first_lba + track.sectors - lba)
            track.stream.seek((track.source.index + lba - track.first_lba) * RAW_SECTOR)
            raw = track.stream.read(count * RAW_SECTOR)
            if len(raw) != count * RAW_SECTOR:
                raise ValueError("Truncated raw track")
            data = bytearray()
            for n in range(count):
                at = n * RAW_SECTOR
                if sector_lba(raw[at:at + 16]) != lba + n:
                    raise ValueError("Raw sector address disagrees with its track")
                data.extend(raw[at + 16:at + 16 + SECTOR])
            take = min(size, len(data))
            yield bytes(data[:take])
            size -= take
            lba += count

    def read(self, lba: int, size: int) -> bytes:
        return b"".join(self.chunks(lba, size))

    def _record(self, data: bytes, at: int):
        length = data[at]
        if length < 34 or at + length > len(data) or at % SECTOR + length > SECTOR:
            raise ValueError("Truncated ISO9660 directory record")
        record = data[at:at + length]
        if record[1] or record[26] or record[27] or record[25] & ~3:
            raise ValueError("Unsupported ISO9660 extended/interleaved/multiextent record")
        if self._both(record, 28, 2) != 1 or 33 + record[32] > length:
            raise ValueError("Invalid ISO9660 file identifier")
        return (self._both(record, 2, 4), self._both(record, 10, 4),
                record[25], record[33:33 + record[32]])

    def _walk(self, lba, size, parent, visited, paths, depth):
        if depth > 32 or size > 16 * 1024 * 1024 or lba in visited:
            raise ValueError("Cyclic or oversized ISO9660 directory")
        visited.add(lba)
        data = self.read(lba, size)
        at = 0
        while at < len(data):
            if data[at] == 0:
                at = (at // SECTOR + 1) * SECTOR
                continue
            child_lba, child_size, flags, identifier = self._record(data, at)
            at += data[at]
            if identifier in (b"\0", b"\1"):
                continue
            name = identifier.decode("ascii")
            name = re.sub(r";[0-9]+$", "", name)
            child = parent / safe_name(name)
            key = child.as_posix().casefold()
            if key in paths or len(paths) >= 100000:
                raise ValueError("Duplicate or excessive ISO9660 entries")
            paths.add(key)
            self.check_extent(child_lba, child_size)
            if flags & 2:
                self._walk(child_lba, child_size, child, visited, paths, depth + 1)
            else:
                self.entries.append(DiscFile(child, child_lba, child_size))


def write_verified(path: Path, chunks) -> str:
    """Create a new file, or verify an identical existing output without changing it."""
    digest = hashlib.sha256()
    if path.is_symlink():
        raise ValueError(f"Refusing symbolic-link output: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    existing = path.exists()
    with path.open("rb" if existing else "xb") as stream:
        for chunk in chunks:
            digest.update(chunk)
            if existing:
                if stream.read(len(chunk)) != chunk:
                    raise ValueError(f"Existing output differs: {path}")
            else:
                stream.write(chunk)
        if existing and stream.read(1):
            raise ValueError(f"Existing output has extra data: {path}")
    return digest.hexdigest()


def extract(cue: Path, output: Path) -> dict:
    output = output.resolve()
    with Disc(cue) as disc:
        rows = []
        for entry in disc.entries:
            destination = output / "disc" / Path(entry.path)
            if not destination.resolve().is_relative_to(output):
                raise ValueError("Output path escapes extraction directory")
            digest = write_verified(destination, disc.chunks(entry.lba, entry.size))
            rows.append(dict(path=entry.path.as_posix(), lba=entry.lba,
                             size=entry.size, sha256=digest))
        write_verified(output / "IP.BIN", [disc.boot])
        result = dict(format="dreamcast-legends-disc-v1", volume=disc.volume,
                      product=disc.boot[64:74].decode("ascii").strip(),
                      title=disc.boot[128:256].decode("ascii").strip(),
                      executable=disc.boot[96:112].decode("ascii").strip(),
                      files=rows,
                      data_tracks=[dict(file=t.source.path.name, index=t.source.index,
                                        first_lba=t.first_lba, sectors=t.sectors)
                                   for t in disc.tracks],
                      scope="Native disc bytes; not converted or certified runtime-compatible")
        write_verified(output / "disc-manifest.json",
                       [(json.dumps(result, indent=2) + "\n").encode()])
        return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cue", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    try:
        if args.list:
            with Disc(args.cue) as disc:
                for entry in disc.entries:
                    print(f"{entry.size:12d} {entry.lba:8d} {entry.path}")
                print(f"{len(disc.entries)} files; {sum(e.size for e in disc.entries)} bytes")
        elif args.output:
            manifest = extract(args.cue, args.output)
            print(f"Extracted and verified {len(manifest['files'])} native disc files")
            native = unpack_native(args.output)
            print(f"Restored {len(native['files'])} named native payloads; "
                  f"{len(native['external'])} external directory references retained in manifest")
        else:
            parser.error("Specify --list or --output")
    except (OSError, ValueError, UnicodeError, struct.error) as error:
        parser.exit(1, f"Extraction failed: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
