#!/usr/bin/env python3
"""Run two to four real native PlayScenes exchanging only inputs over iroh.

Local-only by default; --network-path relay opts into public relay diagnostics
with direct transports disabled. No GPU, desktop control, or saves. Requires an already built
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

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # the top-level helpers
import devenv
from native_determinism import (IMPAIRMENT_DEFAULTS, MAX_TICKS, MAX_RENDER_EVERY, ReportError,
                                compare_reports, validate_impairment)


def input_impairment(options, peers, ticks):
    if options is not None and (not isinstance(options, dict) or set(options) - set(IMPAIRMENT_DEFAULTS)):
        raise ReportError("Unknown input impairment option")
    values = {**IMPAIRMENT_DEFAULTS, **(options or {})}
    enabled = any(values[key] for key in
                  ("latency_ms", "jitter_ms", "loss_ppm", "reorder_ppm", "outage_ms"))
    values.update(mode="simulated-application-input" if enabled else "none",
                  scope="outbound-unreliable-input-only", reliable_impaired=False)
    validate_impairment(values, peers, ticks)
    return values


def run_pair(probe, output, *, ticks, stage, workload, render_every, delay, timeout,
             assets=None, data=None, perturb_host_tick=0, peers=2, impairment=None,
             network_path="local"):
    """Own only these bounded child processes; always reap on error/interruption.

    The historical name remains compatible with two-peer callers. Each process
    owns one explicit seat; guests connect only to the host's private invitation.
    """
    if (type(peers) is not int or not 2 <= peers <= 4 or len(render_every) != peers or
            any(type(cadence) is not int or not 0 <= cadence <= MAX_RENDER_EVERY
                for cadence in render_every)):
        raise ReportError("Require 2..4 peers and exactly one bounded render cadence per peer")
    if network_path not in ("local", "relay"):
        raise ReportError("Network path must be local or relay")
    impairment = input_impairment(impairment, peers, ticks)
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
                           "--ticket", str(ticket), "--input-delay", str(delay),
                           "--network-path", network_path]
                for key in IMPAIRMENT_DEFAULTS:
                    flag = "impairment-seed" if key == "seed" else key.replace("_", "-")
                    command.extend([f"--input-{flag}", str(impairment[key])])
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
                "input_delivery": "iroh-rollback", "input_delay": delay, "impairment": impairment,
                "network_path": network_path}
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
        else:
            result["metrics"].append(compared["metrics"][1])
    result["reports"] = [str(path) for path in paths]
    result["render_every"] = list(render_every)
    result["peers"] = peers
    for index, metrics in enumerate(result["metrics"], 1):
        if metrics["transport"]["local_peer"] != index:
            raise ReportError("Transport path evidence belongs to a different peer")
    if impairment["outage_ms"]:
        actual = result["metrics"][impairment["outage_peer"] - 1]["impairment"]
        if (not actual["outage_finished"] or not actual["outage_dropped"] or
                actual["outage_started_tick"] < impairment["outage_tick"] or
                ticks <= actual["outage_started_tick"] + 8):
            raise ReportError("Requested input outage was not exercised and confirmed beyond recovery")
        progress = actual.get("outage_progress")
        if impairment["outage_ms"] * 60 > 8000 and (
                not progress or not progress["updates"] or
                not (progress["stalls"] or progress["pacing_idles"])):
            raise ReportError("Beyond-window input outage lacks measured outage-window native waits")
        if impairment["outage_ms"] * 60 > 8000 and not actual["oldest_recovery_datagrams"]:
            raise ReportError("Beyond-window input outage did not exercise oldest-input recovery")
        result["outage_recovery"] = {
            "scope": f"simulated-application-input-over-real-{network_path}-iroh",
            "sender_peer": impairment["outage_peer"],
            "started_tick": actual["outage_started_tick"], "all_peers_confirmed_through": ticks,
            "measured_outage_ms": actual["outage_elapsed_ms"],
            "oldest_recovery_datagrams": actual["oldest_recovery_datagrams"],
            "outage_progress": progress,
            "prediction_bound_ticks": 8,
        }
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
    parser.add_argument("--delay", type=int, default=1)
    parser.add_argument("--network-path", choices=("local", "relay"), default="local",
                        help="Opt-in relay uses the public relay service and disables direct transports")
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--perturb-host-tick", type=int, default=0,
                        help="Change only the host's sampled input; guest must receive it over iroh")
    for key, default in IMPAIRMENT_DEFAULTS.items():
        flag = "impairment-seed" if key == "seed" else key.replace("_", "-")
        parser.add_argument(f"--input-{flag}", dest=f"impairment_{key}", type=int, default=default,
                            help="Simulated outbound input impairment; reliable controls and real RTT are unchanged")
    args = parser.parse_args(argv)
    if args.render_every is None:
        args.render_every = list(range(max(0, min(args.peers, 4))))
    if (not 2 <= args.peers <= 4 or len(args.render_every) != args.peers or
            not 1 <= args.ticks <= MAX_TICKS or not 0 <= args.delay <= 2 or
            any(not 0 <= cadence <= MAX_RENDER_EVERY for cadence in args.render_every) or
            not math.isfinite(args.timeout) or args.timeout <= 0 or
            not 0 <= args.perturb_host_tick <= args.ticks):
        parser.error("Invalid peer, tick, delay, cadence count, or timeout bounds")
    impairment = {key: getattr(args, f"impairment_{key}") for key in IMPAIRMENT_DEFAULTS}
    try:
        input_impairment(impairment, args.peers, args.ticks)
    except ReportError as error:
        parser.error(str(error))
    try:
        build = devenv.ROOT / "build" / devenv.release_preset()
        probe = args.probe or build / "bin" / ("nativeprobe.exe" if devenv.WINDOWS else "nativeprobe")
        if not probe.is_file():
            raise ReportError(f"Build nativeprobe first: {probe}")
        if args.output:
            output = args.output.resolve()
            output.mkdir(parents=True, exist_ok=False)
        else:
            output = Path(tempfile.mkdtemp(prefix="native-rollback-", dir=build))
        print(f"Native input-only network reports: {output}", flush=True)
        if args.network_path == "relay":
            print("Public Iroh relay diagnostic: direct transports disabled on every peer; "
                  "each link must provide measured relay-path evidence.", flush=True)
        if input_impairment(impairment, args.peers, args.ticks)["mode"] != "none":
            print("Simulated application-input impairment; provider RTT is measured separately and excludes this delay.",
                  flush=True)
        result = run_pair(probe, output, ticks=args.ticks, stage=args.stage,
                          workload=args.workload, render_every=args.render_every,
                          delay=args.delay, timeout=args.timeout, assets=args.assets, data=args.data,
                          perturb_host_tick=args.perturb_host_tick, peers=args.peers,
                          impairment=impairment, network_path=args.network_path)
        print(json.dumps(result, indent=2, allow_nan=False))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Native network comparison failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
