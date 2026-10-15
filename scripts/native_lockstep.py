#!/usr/bin/env python3
"""Run two to four real native PlayScenes exchanging only inputs over local iroh.

No GPU, desktop control, public relay, or saves. Requires an already built
nativeprobe. Retains reports/logs and a PRIVATE session ticket under build/.
Hashes certify only the observation coverage explicitly listed in the reports.
Sample-to-update timing is not display latency or an Internet latency estimate.
"""

import argparse
import contextlib
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import time

import devenv
from native_determinism import MAX_TICKS, MAX_RENDER_EVERY, ReportError, compare_reports


def run_pair(probe, output, *, ticks, stage, workload, render_every, delay, timeout,
             assets=None, data=None, perturb_host_tick=0, peers=2):
    """Own only these bounded child processes; always reap on error/interruption.

    The historical name remains compatible with two-peer callers. Each process
    owns one explicit seat; guests connect only to the host's private invitation.
    """
    if (type(peers) is not int or not 2 <= peers <= 4 or len(render_every) != peers or
            any(type(cadence) is not int or not 0 <= cadence <= MAX_RENDER_EVERY
                for cadence in render_every)):
        raise ReportError("Require 2..4 peers and exactly one bounded render cadence per peer")
    probe, output = Path(probe).resolve(), Path(output).resolve()
    ticket = output / "private-ticket.txt"
    paths = [output / f"run-{index}.jsonl" for index in range(1, peers + 1)]
    logs = [output / f"run-{index}.log" for index in range(1, peers + 1)]
    if any(path.exists() for path in [ticket, *paths, *logs]):
        raise ReportError("Refusing existing native network probe output")
    processes = []
    deadline = time.monotonic() + timeout
    with contextlib.ExitStack() as stack:
        try:
            for index in range(peers):
                role = "host" if index == 0 else "guest"
                command = [str(probe), "--report", str(paths[index]), "--stage", stage,
                           "--ticks", str(ticks), "--players", str(peers), "--workload", workload,
                           "--render-every", str(render_every[index]), "--network-role", role,
                           "--network-peer", str(index + 1),
                           "--ticket", str(ticket), "--input-delay", str(delay)]
                if role == "host" and perturb_host_tick:
                    command.extend(["--perturb-tick", str(perturb_host_tick)])
                for flag, value in (("assets", assets), ("data", data)):
                    if value is not None:
                        command.extend([f"--{flag}", str(Path(value).resolve())])
                stream = stack.enter_context(logs[index].open("x", encoding="utf-8"))
                processes.append(subprocess.Popen(
                    command, cwd=devenv.ROOT, stdin=subprocess.DEVNULL,
                    stdout=stream, stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW if devenv.WINDOWS else 0))
                if role == "host":
                    while not ticket.is_file():
                        if processes[0].poll() is not None:
                            raise ReportError(f"Host ended before invitation; inspect {logs[0]}")
                        if time.monotonic() >= deadline:
                            raise ReportError("Native network invitation timed out")
                        time.sleep(0.02)
            while any(process.poll() is None for process in processes):
                if any(process.poll() not in (None, 0) for process in processes):
                    raise ReportError(f"Native network process failed; inspect {output}")
                if time.monotonic() >= deadline:
                    raise ReportError("Native network run timed out")
                time.sleep(0.02)
            if any(process.returncode != 0 for process in processes):
                raise ReportError(f"Native network process failed; inspect {output}")
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
            for process in processes:
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    expected = {"ticks": ticks, "players": peers, "stage": stage, "workload": workload,
                "input_delivery": "iroh-lockstep", "input_delay": delay}
    result = None
    for index in range(1, peers):
        try:
            compared = compare_reports(paths[0], paths[index],
                                       {**expected, "render_every": render_every[0]},
                                       {**expected, "render_every": render_every[index]})
        except (OSError, ReportError) as error:
            raise ReportError(f"Peer {index + 1} versus host: {error}") from error
        if result is None:
            result = compared
    result["reports"] = [str(path) for path in paths]
    result["render_every"] = list(render_every)
    result["peers"] = peers
    return result


def main(argv=None):
    import json
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--assets", type=Path)
    parser.add_argument("--data", type=Path)
    parser.add_argument("--ticks", type=int, default=600)
    parser.add_argument("--stage", default="G1")
    parser.add_argument("--workload", choices=("idle", "combat"), default="combat")
    parser.add_argument("--peers", type=int, default=2)
    parser.add_argument("--render-every", type=int, nargs="+",
                        help="One cadence per peer; defaults to 0,1,...")
    parser.add_argument("--delay", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--perturb-host-tick", type=int, default=0,
                        help="Change only the host's sampled input; guest must receive it over iroh")
    args = parser.parse_args(argv)
    if args.render_every is None:
        args.render_every = list(range(max(0, min(args.peers, 4))))
    if (not 2 <= args.peers <= 4 or len(args.render_every) != args.peers or
            not 1 <= args.ticks <= MAX_TICKS or not 0 <= args.delay <= 6 or
            any(not 0 <= cadence <= MAX_RENDER_EVERY for cadence in args.render_every) or
            not math.isfinite(args.timeout) or args.timeout <= 0 or
            not 0 <= args.perturb_host_tick <= args.ticks):
        parser.error("Invalid peer, tick, delay, cadence count, or timeout bounds")
    try:
        build = devenv.ROOT / "build" / devenv.release_preset()
        probe = args.probe or build / "bin" / ("nativeprobe.exe" if devenv.WINDOWS else "nativeprobe")
        if not probe.is_file():
            raise ReportError(f"Build nativeprobe first: {probe}")
        if args.output:
            output = args.output.resolve()
            output.mkdir(parents=True, exist_ok=False)
        else:
            output = Path(tempfile.mkdtemp(prefix="native-lockstep-", dir=build))
        print(f"Native input-only network reports: {output}", flush=True)
        result = run_pair(probe, output, ticks=args.ticks, stage=args.stage,
                          workload=args.workload, render_every=args.render_every,
                          delay=args.delay, timeout=args.timeout, assets=args.assets, data=args.data,
                          perturb_host_tick=args.perturb_host_tick, peers=args.peers)
        print(json.dumps(result, indent=2, allow_nan=False))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Native network comparison failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
