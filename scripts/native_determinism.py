#!/usr/bin/env python3
"""Compare bounded native PlayScene observations from two independent processes.

    python scripts/native_determinism.py --ticks 600 --render-every 0 1
    python scripts/native_determinism.py --compare first.jsonl second.jsonl

Runs are sequential, headless, and use the existing nativeprobe executable;
this script never builds it. Reports and process logs are retained. Equal hashes
prove only observed-equal state for these inputs, not complete determinism.
"""

import argparse
import contextlib
import json
import math
import pathlib
import re
import subprocess
import sys
import tempfile

import devenv


MAX_TICKS = 36_000
MAX_RENDER_EVERY = 60
MAX_LINE_BYTES = 1024 * 1024
MAX_SUBSYSTEMS = 256
HASH = re.compile(r"[0-9a-f]{64}\Z")
METADATA_FIELDS = {
    "type", "schema", "simulation", "stage", "ticks", "players", "tick_rate",
    "render_every", "workload", "input_version", "initialization",
    "input_delivery", "input_delay",
    "complete_state", "unobserved", "subsystems", "assets_digest", "data_digest", "config_digest",
}


class ReportError(ValueError):
    """Missing, malformed, incomplete, or unequal native observations."""


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ReportError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def _nonfinite(value):
    raise ReportError(f"Nonfinite JSON number: {value}")


def _integer(value, minimum, maximum):
    return type(value) is int and minimum <= value <= maximum


def _name(value):
    return isinstance(value, str) and 0 < len(value) <= 128 and not any(
        ord(character) < 32 for character in value)


def _names(value, nonempty=False, ordered=False):
    return (isinstance(value, list) and int(nonempty) <= len(value) <= MAX_SUBSYSTEMS and
            all(_name(name) for name in value) and len(set(value)) == len(value) and
            (not ordered or value == sorted(value)))


def _descriptions(value):
    return (isinstance(value, list) and len(value) <= MAX_SUBSYSTEMS and
            all(isinstance(description, str) and description.strip() and
                len(description) <= 1024 and
                not any(ord(character) < 32 for character in description)
                for description in value) and len(set(value)) == len(value))


class _Reader:
    def __init__(self, path, stream):
        self.path = path
        self.stream = stream
        self.line = 0

    def fail(self, message):
        raise ReportError(f"{self.path}:{self.line}: {message}")

    def row(self):
        line = self.stream.readline(MAX_LINE_BYTES + 1)
        self.line += 1
        if not line:
            self.fail("Missing row (empty or truncated report)")
        if len(line) > MAX_LINE_BYTES:
            self.fail("Report line exceeds byte bound")
        if not line.endswith(b"\n"):
            self.fail("Unterminated JSONL row (possibly truncated report)")
        try:
            row = json.loads(line.decode("utf-8"), object_pairs_hook=_unique_object,
                             parse_constant=_nonfinite)
        except (ValueError, UnicodeError, RecursionError) as error:
            self.fail(f"Invalid JSON: {error}")
        if not isinstance(row, dict):
            self.fail("Expected a JSON object")
        return row

    def metadata(self, expected):
        row = self.row()
        if set(row) != METADATA_FIELDS:
            self.fail("Missing or unknown metadata fields")
        exact = {"type": "metadata", "simulation": "PlayScene::update",
                 "initialization": "native-defaults-v1", "complete_state": False}
        if any(type(row[key]) is not type(value) or row[key] != value
               for key, value in exact.items()):
            self.fail("Unsupported simulation, initialization, or observation scope")
        for key, low, high in (("schema", 1, 1), ("input_version", 1, 1),
                               ("tick_rate", 60, 60), ("ticks", 1, MAX_TICKS),
                               ("players", 1, 4), ("render_every", 0, MAX_RENDER_EVERY)):
            if not _integer(row[key], low, high):
                self.fail(f"Invalid metadata {key}")
        if (row["input_delivery"] not in ("local", "iroh-lockstep") or
                not _integer(row["input_delay"], 0, 6) or
                (row["input_delivery"] == "local" and row["input_delay"] != 0) or
                (row["input_delivery"] == "iroh-lockstep" and row["players"] < 2)):
            self.fail("Invalid native input delivery or delay")
        if (not _name(row["stage"]) or row["workload"] not in ("idle", "combat") or
                not _names(row["subsystems"], nonempty=True, ordered=True) or
                not _descriptions(row["unobserved"])):
            self.fail("Invalid stage, workload, or observation roster")
        if set(row["subsystems"]) & set(row["unobserved"]):
            self.fail("Observed and unobserved rosters overlap")
        if any(not isinstance(row[key], str) or not HASH.fullmatch(row[key])
               for key in ("assets_digest", "data_digest", "config_digest")):
            self.fail("Invalid assets, data, or config digest")
        for key, value in (expected or {}).items():
            if key not in row or type(row[key]) is not type(value) or row[key] != value:
                self.fail(f"Requested {key} differs from report")
        return row

    def tick(self, number, names):
        row = self.row()
        if (set(row) != {"type", "tick", "input_digest", "subsystems", "unavailable"} or
                row["type"] != "tick" or not _integer(row["tick"], number, number)):
            self.fail(f"Expected contiguous tick {number}")
        digest, hashes, unavailable = row["input_digest"], row["subsystems"], row["unavailable"]
        if not isinstance(digest, str) or not HASH.fullmatch(digest):
            self.fail(f"Invalid input digest at tick {number}")
        if not isinstance(hashes, dict) or not isinstance(unavailable, dict):
            self.fail(f"Invalid subsystem map at tick {number}")
        if (set(hashes) & set(unavailable) or set(hashes) | set(unavailable) != set(names) or
                any(not isinstance(value, str) or not HASH.fullmatch(value)
                    for value in hashes.values()) or
                any(not isinstance(reason, str) or not reason.strip() or len(reason) > 4096
                    for reason in unavailable.values())):
            self.fail(f"Invalid subsystem roster or digest at tick {number}")
        if unavailable:
            name = sorted(unavailable)[0]
            self.fail(f"Unavailable subsystem {name} at tick {number}: {unavailable[name]}")
        return row

    def finish(self, ticks):
        row = self.row()
        if (set(row) != {"type", "ticks", "last_tick"} or row["type"] != "complete" or
                not _integer(row["ticks"], ticks, ticks) or
                not _integer(row["last_tick"], ticks, ticks)):
            self.fail("Missing or inconsistent completion footer")
        if self.stream.read(1):
            self.fail("Unexpected data after completion footer")


def compare_reports(left, right, expected_left=None, expected_right=None):
    """Validate complete reports in bounded memory and identify the first mismatch."""
    paths = [pathlib.Path(left), pathlib.Path(right)]
    if paths[0].resolve() == paths[1].resolve():
        raise ReportError("Two distinct reports are required")
    with contextlib.ExitStack() as stack:
        readers = [_Reader(path, stack.enter_context(path.open("rb"))) for path in paths]
        metadata = [reader.metadata(expected) for reader, expected in
                    zip(readers, (expected_left, expected_right))]
        for key in sorted(METADATA_FIELDS - {"render_every"}):
            if metadata[0][key] != metadata[1][key]:
                raise ReportError(f"Metadata differs: {key}")
        first_difference = None
        names = metadata[0]["subsystems"]
        ticks = metadata[0]["ticks"]
        for number in range(1, ticks + 1):
            a, b = (reader.tick(number, names) for reader in readers)
            if a["input_digest"] != b["input_digest"] and first_difference is None:
                first_difference = f"First difference at tick {number}, input_digest"
            for name in names:
                lhs, rhs = a["subsystems"][name], b["subsystems"][name]
                if lhs != rhs and first_difference is None:
                    first_difference = (f"First difference at tick {number}, subsystem {name}: "
                                        f"{lhs} != {rhs}")
        for reader in readers:
            reader.finish(ticks)
        if first_difference:
            raise ReportError(first_difference)
    return {"status": "observed-equal", "complete_state": False,
            "metadata": {key: value for key, value in metadata[0].items()
                         if key not in ("type", "render_every")},
            "render_every": [value["render_every"] for value in metadata],
            "reports": [str(path.resolve()) for path in paths]}


def run_pair(probe, output, *, ticks, players, stage, workload, render_every,
             timeout, assets=None, data=None):
    """Run two independent probes sequentially; never reuse an existing report."""
    probe, output = pathlib.Path(probe).resolve(), pathlib.Path(output).resolve()
    if len(render_every) != 2:
        raise ReportError("Exactly two render cadences are required")
    reports, expectations = [], []
    for index, cadence in enumerate(render_every, start=1):
        report = output / f"run-{index}.jsonl"
        log = output / f"run-{index}.log"
        if report.exists():
            raise ReportError(f"Refusing an existing report: {report}")
        expected = {"ticks": ticks, "players": players, "stage": stage,
                    "workload": workload, "render_every": cadence}
        command = [str(probe), "--report", str(report), "--stage", stage,
                   "--ticks", str(ticks), "--players", str(players),
                   "--render-every", str(cadence), "--workload", workload]
        for flag, path in (("assets", assets), ("data", data)):
            if path is not None:
                command.extend([f"--{flag}", str(pathlib.Path(path).resolve())])
        with log.open("x", encoding="utf-8") as stream:
            result = subprocess.run(command, cwd=devenv.ROOT, stdin=subprocess.DEVNULL,
                                    stdout=stream, stderr=subprocess.STDOUT, timeout=timeout,
                                    check=False, creationflags=(subprocess.CREATE_NO_WINDOW
                                                               if devenv.WINDOWS else 0))
        if result.returncode != 0:
            raise ReportError(f"Probe {index} failed (exit {result.returncode}); inspect {log}")
        if not report.is_file() or report.stat().st_size == 0:
            raise ReportError(f"Probe {index} produced no observations; inspect {log}")
        reports.append(report)
        expectations.append(expected)
    return compare_reports(*reports, *expectations)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--compare", type=pathlib.Path, nargs=2, metavar=("LEFT", "RIGHT"))
    mode.add_argument("--probe", type=pathlib.Path, help="Existing nativeprobe executable")
    parser.add_argument("--build-dir", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, help="New directory for retained reports/logs")
    parser.add_argument("--assets", type=pathlib.Path)
    parser.add_argument("--data", type=pathlib.Path)
    parser.add_argument("--stage")
    parser.add_argument("--ticks", type=int)
    parser.add_argument("--players", type=int)
    parser.add_argument("--workload", choices=("idle", "combat"))
    parser.add_argument("--render-every", type=int, nargs=2, metavar=("FIRST", "SECOND"),
                        help="Draw every K simulation ticks (0 disables rendering; default: 0 1)")
    parser.add_argument("--timeout", type=float, help="Per-process timeout seconds (default: 300)")
    args = parser.parse_args(argv)
    try:
        if args.compare:
            if any(value is not None for key, value in vars(args).items()
                   if key not in ("compare", "probe")):
                parser.error("--compare does not accept process-launch options")
            result = compare_reports(*args.compare)
        else:
            ticks = 600 if args.ticks is None else args.ticks
            players = 2 if args.players is None else args.players
            cadence = [0, 1] if args.render_every is None else args.render_every
            timeout = 300 if args.timeout is None else args.timeout
            stage = "G1" if args.stage is None else args.stage
            if (not _integer(ticks, 1, MAX_TICKS) or not _integer(players, 1, 4) or
                    any(not _integer(value, 0, MAX_RENDER_EVERY) for value in cadence) or
                    not math.isfinite(timeout) or timeout <= 0 or not _name(stage)):
                parser.error("Invalid ticks, players, render cadence, timeout, or stage")
            directory = (args.build_dir or devenv.ROOT / "build" / devenv.release_preset()).resolve()
            probe = (args.probe or directory / "bin" /
                     ("nativeprobe.exe" if devenv.WINDOWS else "nativeprobe")).resolve()
            if not probe.is_file():
                raise ReportError(f"nativeprobe is missing; build it first: {probe}")
            if args.output is not None:
                output = args.output.resolve()
                output.mkdir(parents=True, exist_ok=False)
            else:
                output = pathlib.Path(tempfile.mkdtemp(prefix="native-determinism-", dir=directory))
            print(f"Reports: {output}", flush=True)
            result = run_pair(probe, output, ticks=ticks, players=players, stage=stage,
                              workload=args.workload or "idle", render_every=cadence,
                              timeout=timeout, assets=args.assets, data=args.data)
        print(json.dumps(result, indent=2, allow_nan=False))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Native observation comparison failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
