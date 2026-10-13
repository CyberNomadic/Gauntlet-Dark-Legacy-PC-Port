#!/usr/bin/env python3
"""Measure native gameplay over real iroh without opening windows or reading input.

Local A/B comparison (not proof of Internet routing):
  python scripts/netplay_latency.py --build --fps 30 60
Native Archer turbo volleys, including release-to-projectile submission:
  python scripts/netplay_latency.py --workload turbo-B --character 3 --require-volleys --seconds 25 --fps 30 60

Separate computers, each with its own assets:
  python scripts/netplay_latency.py --role host --invitation <new-private-file> --internet
  Transfer that private invitation file to the guest, then:
  python scripts/netplay_latency.py --role guest --invitation <private-file> --internet

Reports contain no tickets/addresses. Direct/relay use comes from iroh's selected
path, not from --internet. Headless visual-submission timing excludes audio,
GPU/scanout and physical input polling. A blocked movement start is counted as
missed, never as a zero.
"""
import argparse
import contextlib
import json
import math
import pathlib
import subprocess
import sys
import tempfile
import time

import devenv
from netplay_test import stop


def evaluate_turbo(report, limit_ms, send_limit_ms, turbo_limit_ms):
    """Sparse action observation, not an input-acceptance or damage acknowledgement."""
    failures = []
    if report.get("schema") != 3 or report.get("turbo_probe_schema") != 1:
        failures.append("No turbo action-observation schema")
    if report.get("workload") not in ("turbo-B", "turbo-C", "shove"):
        failures.append("Unknown turbo workload")
    for key in ("rtt", "sample_to_send", "sample_to_resolved", "update_interval", "render_interval"):
        count = report.get(key, {}).get("count", 0)
        if type(count) is not int or count <= 0:
            failures.append(f"No samples for {key}")
    for key in ("turbo_to_first_frame", "turbo_to_host_action"):
        values = report.get(key, {})
        count = values.get("count", 0)
        if type(count) is not int or count < 3:
            failures.append(f"Fewer than three observations for {key}")
        latency = values.get("p95_ms")
        if type(latency) not in (int, float) or not math.isfinite(latency) or latency < 0:
            failures.append(f"No valid 95th-percentile timing for {key}")
    for key in ("pending_turbo_requests", "expired_turbo_requests", "interrupted_turbo_requests",
                "omitted_turbo_requests"):
        count = report.get(key)
        if type(count) is not int or count < 0:
            failures.append(f"No valid counter for {key}")
        elif count:
            failures.append(f"{key}: {count}")
    completed = report.get("complete_turbo_requests")
    requested = report.get("turbo_requests")
    if (type(completed) is not int or completed < 3 or type(requested) is not int or
            requested != completed):
        failures.append("Turbo request/completion counts do not match at least three complete requests")
    for key in ("turbo_to_first_frame", "turbo_to_host_action"):
        if report.get(key, {}).get("count") != completed:
            failures.append(f"Incomplete observation count for {key}")
    latency = report.get("turbo_to_first_frame", {}).get("p95_ms")
    if (limit_ms is not None and type(latency) in (int, float) and latency > turbo_limit_ms):
        failures.append(f"95th-percentile turbo startup-to-draw exceeds {turbo_limit_ms:g} ms")
    send = report.get("sample_to_send", {}).get("p95_ms")
    if type(send) not in (int, float) or not math.isfinite(send) or send < 0:
        failures.append("No valid sample-to-send timing")
    elif send > send_limit_ms:
        failures.append(f"95th-percentile local send queue exceeds {send_limit_ms:g} ms")
    fps = report.get("render_fps", 0)
    if fps not in (30, 60, 144):
        failures.append("No valid presentation rate recorded")
    else:
        measured = report.get("render_interval", {}).get("median_ms")
        if type(measured) not in (int, float) or not math.isfinite(measured) or measured < 0:
            failures.append("No valid presentation interval recorded")
        elif measured > 1250 / fps:
            failures.append("Measured presentation rate fell substantially below its target")
    measured = report.get("update_interval", {}).get("median_ms")
    if type(measured) not in (int, float) or not math.isfinite(measured) or measured < 0:
        failures.append("No valid simulation interval recorded")
    elif measured > 1250 / 60:
        failures.append("Measured simulation rate fell substantially below 60 Hz")
    return failures


def evaluate_volleys(report, release_limit_ms):
    """Require exact native volley identities on both paths, never infer hits."""
    failures = []
    if (type(report.get("turbo_volley_probe_schema")) is not int or
            report.get("turbo_volley_probe_schema") != 1 or
            report.get("projectile_timing_scope") != "turbo-volley-release" or
            report.get("timing_scope") != "visual-submit-no-audio"):
        failures.append("Missing turbo volley visual-observation schema")
    for key in ("displayed_volley_projectiles", "host_volley_projectiles", "matched_volley_projectiles"):
        count = report.get(key)
        if type(count) is not int or count < 3:
            failures.append(f"Fewer than three distinct {key}")
    for prefix in ("displayed_", "host_"):
        if report.get(prefix + "volley_overflow") is not False:
            failures.append(f"Missing or saturated {prefix}volley identity history")
        count = report.get(prefix + "invalid_volley_projectiles")
        if type(count) is not int or count != 0:
            failures.append(f"Invalid {prefix}volley identity observations")
    for key in ("release_to_first_projectile_frame", "release_to_host_projectile"):
        values = report.get(key)
        values = values if isinstance(values, dict) else {}
        count = values.get("count")
        if type(count) is not int or count < 3:
            failures.append(f"Fewer than three observations for {key}")
        latency = values.get("p95_ms")
        if type(latency) not in (int, float) or not math.isfinite(latency) or latency < 0:
            failures.append(f"No valid 95th-percentile timing for {key}")
        elif key == "release_to_first_projectile_frame" and release_limit_ms is not None and latency > release_limit_ms:
            failures.append(f"95th-percentile native release-to-projectile draw exceeds {release_limit_ms:g} ms")
    for key in ("pending_releases", "expired_releases", "interrupted_releases", "unobserved_host_projectiles"):
        count = report.get(key)
        if type(count) is not int or count < 0:
            failures.append(f"No valid counter for {key}")
        elif count:
            failures.append(f"{key}: {count}")
    # A fast ordinary throw or three frames of one projectile cannot certify
    # turbo. Retained detailed samples must contain distinct completed emissions.
    records = report.get("release_samples")
    identities = set()
    invalid = not isinstance(records, list)
    for record in records if isinstance(records, list) else ():
        if not isinstance(record, dict):
            invalid = True
            continue
        fields = tuple(record.get(key) for key in
                       ("epoch", "tick", "generation", "grant", "seat", "emission"))
        if (any(type(value) is not int for value in fields) or
                fields[0] <= 0 or fields[1] < 0 or fields[2] <= 0 or not 0 < fields[3] < 1 << 32 or
                any(value >= 1 << 64 for value in fields[:3]) or not 0 <= fields[4] < 4 or
                not 1 <= fields[5] >> 16 <= 128 or fields[5] >= 1 << 32 or
                record.get("strong") is not False or type(record.get("fan_ordinal")) is not int or
                record.get("fan_ordinal") != 0 or record.get("outcome") != "complete" or
                any(type(record.get(key)) not in (int, float) or
                    not math.isfinite(record[key]) or record[key] < 0
                    for key in ("first_frame_ms", "host_ms"))):
            invalid = True
            continue
        if fields in identities:
            invalid = True
        identities.add(fields)
    if invalid:
        failures.append("Invalid or incomplete turbo volley release samples")
    if len(identities) < 3:
        failures.append("Fewer than three distinct timed turbo volley identities")
    return failures


def evaluate(report, limit_ms, throw_limit_ms=200, send_limit_ms=10, release_limit_ms=75,
             turbo_limit_ms=75):
    """Require animation and projectile evidence; None motion budget denotes the unpredicted baseline."""
    if report.get("status") != "complete" or report.get("role") != "guest":
        return ["No completed guest report"]
    character = report.get("character", 0)
    if type(character) is not int or not 0 <= character < 8:
        return ["No valid base-class index recorded"]
    require_volleys = report.get("require_volleys", False)
    if type(require_volleys) is not bool:
        return ["No valid volley requirement recorded"]
    if require_volleys and report.get("workload") not in ("turbo-B", "turbo-C"):
        return ["Volley observations require turbo-B or turbo-C"]
    element = report.get("element", "none")
    elements = {"none": 0, "fire": 1, "lightning": 2, "light": 3, "acid": 4}
    if report.get("workload", "throw") not in ("throw", "rapid-five", "speed-rapid-five"):
        failures = evaluate_turbo(report, limit_ms, send_limit_ms, turbo_limit_ms)
        if element != "none":
            failures.append("Elemental amulets require a throw workload")
        if require_volleys:
            failures.extend(evaluate_volleys(report, release_limit_ms if limit_ms is not None else None))
        return failures
    failures = []
    if type(element) is not str or element not in elements:
        failures.append("No valid elemental loadout recorded")
    elif element != "none":
        if (type(report.get("element_probe_schema")) is not int or report.get("element_probe_schema") != 1 or
                type(report.get("element_flags")) is not int or report.get("element_flags") != elements[element]):
            failures.append("Missing matching elemental observation schema")
        if report.get("timing_scope") != "visual-submit-no-audio":
            failures.append("Elemental workload must identify visual-only measurement scope")
        certified = report.get("element_certified_observations")
        if type(certified) is not int or certified < 3:
            failures.append("Fewer than three host-certified elemental observations")
        for prefix in ("displayed_", "host_"):
            count = report.get(prefix + "element_projectiles")
            if type(count) is not int or count < 3:
                failures.append(f"Fewer than three {prefix}elemental projectiles")
            invalid = report.get(prefix + "invalid_element_projectiles")
            if type(invalid) is not int or invalid != 0:
                failures.append(f"Invalid {prefix}elemental projectile observations")
    if report.get("workload") in ("rapid-five", "speed-rapid-five"):
        if report.get("item_probe_schema") != 1 or report.get("projectile_timing_scope") != "center-shot":
            failures.append("Missing rapid-fire fan observation schema")
        certified = report.get("certified_item_observations")
        if type(certified) is not int or certified <= 0:
            failures.append("No host-certified rapid-fire five-way modifiers observed")
        for prefix in ("displayed_", "host_"):
            complete = report.get(prefix + "complete_fans")
            partial = report.get(prefix + "partial_fans")
            if type(complete) is not int or complete < 3:
                failures.append(f"Fewer than three complete {prefix}fans")
            if type(partial) is not int or partial != 0:
                failures.append(f"Incomplete {prefix}fan observations")
    if report.get("workload") == "speed-rapid-five":
        if report.get("speed_probe_schema") != 1:
            failures.append("Missing Speed observation schema")
        if report.get("timing_scope") != "visual-submit-no-audio":
            failures.append("Speed workload must identify visual-only measurement scope")
        for prefix in ("displayed_", "host_"):
            for suffix in ("speed_certified_observations", "speed_motion_observations"):
                count = report.get(prefix + suffix)
                if type(count) is not int or count < 3:
                    failures.append(f"Fewer than three {prefix + suffix}")
            minimum = report.get(prefix + "speed_minimum_pace")
            maximum = report.get(prefix + "speed_maximum_pace")
            # The host provisions native kind-7 Speed with +3 pace. Its effective
            # range is therefore 8..12.5, after PlayerActor's ordinary clamp.
            if (type(minimum) not in (int, float) or type(maximum) not in (int, float) or
                    not math.isfinite(minimum) or not math.isfinite(maximum) or
                    not 8 <= minimum <= maximum <= 12.5):
                failures.append(f"No valid boosted {prefix}effective pace range")
    if report.get("schema") != 3:
        failures.append("No projectile-aware telemetry schema 3")
    metrics = {key: report[key] if isinstance(report.get(key), dict) else {}
               for key in ("rtt", "sample_to_send", "sample_to_resolved", "motion_to_first_frame",
                           "throw_to_first_frame", "throw_to_authority", "update_interval", "render_interval",
                           "release_to_first_projectile_frame", "release_to_host_projectile")}
    for key, values in metrics.items():
        count = values.get("count")
        if type(count) is not int or count <= 0:
            failures.append(f"No samples for {key}")
    for key, label in (("motion_to_first_frame", "observed movement starts"),
                       ("throw_to_first_frame", "observed throws"),
                       ("throw_to_authority", "host-confirmed throws")):
        count = metrics[key].get("count")
        if type(count) is not int or count < 3:
            failures.append(f"Fewer than three {label}")
    if report.get("missed_motion", 0):
        failures.append(f"{report['missed_motion']} movement starts timed out or were interrupted")
    if report.get("missed_throws", 0):
        failures.append(f"{report['missed_throws']} throws timed out or were interrupted")
    for key in ("pending_throw_authority", "expired_throw_authority",
                "interrupted_throw_authority", "superseded_throw_authority"):
        count = report.get(key)
        if type(count) is not int or count < 0:
            failures.append(f"No valid counter for {key}")
        elif count:
            failures.append(f"{key}: {count}")
    for key in ("release_to_first_projectile_frame", "release_to_host_projectile"):
        count = metrics[key].get("count")
        if type(count) is not int or count < 3:
            failures.append(f"Fewer than three observations for {key}")
    timings = {}
    for key in ("sample_to_send", "motion_to_first_frame", "throw_to_first_frame",
                "throw_to_authority", "release_to_first_projectile_frame", "release_to_host_projectile"):
        latency = metrics[key].get("p95_ms")
        if type(latency) not in (int, float) or not math.isfinite(latency) or latency < 0:
            failures.append(f"No valid 95th-percentile timing for {key}")
        else:
            timings[key] = latency
    for key in ("pending_releases", "expired_releases", "interrupted_releases",
                "unobserved_host_projectiles"):
        count = report.get(key)
        if type(count) is not int or count < 0:
            failures.append(f"No valid counter for {key}")
        elif count:
            failures.append(f"{key}: {count}")
    release_latency = timings.get("release_to_first_projectile_frame")
    if (limit_ms is not None and release_limit_ms is not None and
            type(release_latency) in (int, float) and release_latency > release_limit_ms):
        failures.append(f"95th-percentile native release-to-projectile draw exceeds {release_limit_ms:g} ms")
    if timings.get("throw_to_first_frame", 0) > throw_limit_ms:
        failures.append(f"95th-percentile throw response exceeds {throw_limit_ms:g} ms")
    if timings.get("sample_to_send", 0) > send_limit_ms:
        failures.append(f"95th-percentile local send queue exceeds {send_limit_ms:g} ms")
    if limit_ms is not None and timings.get("motion_to_first_frame", 0) > limit_ms:
        failures.append(f"95th-percentile motion response exceeds {limit_ms:g} ms")
    fps = report.get("render_fps", 0)
    if fps not in (30, 60, 144):
        failures.append("No valid presentation rate recorded")
    for key in ("render_interval", "update_interval"):
        measured = metrics[key].get("median_ms")
        if type(measured) not in (int, float) or not math.isfinite(measured) or measured < 0:
            failures.append(f"No valid median timing for {key}")
        elif key == "render_interval" and fps in (30, 60, 144) and measured > 1250 / fps:
            failures.append("Measured presentation rate fell substantially below its target")
        elif key == "update_interval" and measured > 1250 / 60:
            failures.append("Measured simulation rate fell substantially below 60 Hz")
    return failures


def launch(stack, executable, role, invitation, output, args, fps, prediction):
    report = output / f"{role}.json"
    command = [str(executable), role, str(invitation), str(report), "--seconds", str(args.seconds),
               "--fps", str(fps), "--prediction", prediction, "--players", str(args.players),
               "--workload", args.workload, "--element", args.element,
               "--character", str(getattr(args, "character", 0))]
    if getattr(args, "require_volleys", False):
        command.append("--require-volleys")
    if args.internet:
        command.append("--internet")
    for key in ("assets", "data"):
        if value := getattr(args, key):
            command.extend([f"--{key}", str(value.resolve())])
    stream = stack.enter_context((output / f"{role}.log").open("w", encoding="utf-8"))
    process = subprocess.Popen(command, cwd=devenv.ROOT, stdin=subprocess.DEVNULL,
                               stdout=stream, stderr=subprocess.STDOUT,
                               creationflags=subprocess.CREATE_NO_WINDOW if devenv.WINDOWS else 0)
    stack.callback(stop, process)
    return process, report


def wait_invitation(process, path):
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("Host exited before creating invitation; inspect its log")
        if path.is_file() and path.stat().st_size > 6:
            return
        time.sleep(0.05)
    raise RuntimeError("Invitation timed out; inspect host log")


def read_report(process, path, timeout):
    code = process.wait(timeout=timeout)
    if code != 0 or not path.is_file():
        raise RuntimeError(f"Probe failed ({code}); inspect {path.with_suffix('.log')}")
    return json.loads(path.read_text(encoding="utf-8"))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--build-dir", type=pathlib.Path,
                        default=devenv.ROOT / "build" / devenv.release_preset())
    parser.add_argument("--role", choices=("pair", "host", "guest"), default="pair")
    parser.add_argument("--invitation", type=pathlib.Path)
    parser.add_argument("--internet", action="store_true")
    parser.add_argument("--assets", type=pathlib.Path)
    parser.add_argument("--data", type=pathlib.Path)
    parser.add_argument("--seconds", type=int, choices=range(8, 301), metavar="8..300", default=20)
    parser.add_argument("--players", type=int, choices=(2, 3, 4), default=2)
    parser.add_argument("--fps", type=int, nargs="+", choices=(30, 60, 144), default=[60])
    parser.add_argument("--prediction", choices=("on", "off", "both"), default="both")
    parser.add_argument("--workload", choices=("throw", "rapid-five", "speed-rapid-five", "turbo-B", "turbo-C", "shove"), default="throw",
                        help="Native input workload; meters/items are provisioned only by the host probe")
    parser.add_argument("--element", choices=("none", "fire", "lightning", "light", "acid"), default="none",
                        help="Native elemental amulet added to the throw workload (default: none)")
    parser.add_argument("--character", type=int, choices=range(8), default=0,
                        help="Base class: 0 WAR, 1 VAL, 2 WIZ, 3 ARC, 4 DWF, 5 KNI, 6 SOR, 7 JES")
    parser.add_argument("--require-volleys", action="store_true",
                        help="Require actual turbo volley projectiles and release timings (e.g. --character 3)")
    parser.add_argument("--max-motion-ms", type=float, default=75,
                        help="p95 draw-submission budget for prediction-on runs (default: 75 ms)")
    parser.add_argument("--max-throw-ms", type=float, default=200,
                        help="p95 throw startup-to-draw budget, including local replay (default: 200 ms)")
    parser.add_argument("--max-send-ms", type=float, default=10,
                        help="p95 input sample-to-transport budget (default: 10 ms)")
    parser.add_argument("--max-release-ms", type=float, default=75,
                        help="p95 native release-to-projectile draw budget for prediction-on runs (default: 75 ms)")
    parser.add_argument("--max-turbo-ms", type=float, default=75,
                        help="p95 turbo startup-to-draw budget for prediction-on runs (default: 75 ms)")
    args = parser.parse_args(argv)
    if args.role != "pair" and (not args.invitation or len(args.fps) != 1):
        parser.error("Separate-machine runs require --invitation and one --fps value")
    if args.role == "pair" and args.players != 2:
        parser.error("Use separate host/guest runs for three or four players")
    if args.role == "pair" and args.invitation:
        parser.error("--invitation is only for separate-machine runs; pair mode owns its temporary file")
    if args.workload not in ("throw", "rapid-five", "speed-rapid-five") and args.seconds < 20:
        parser.error("Turbo workloads need at least twenty seconds")
    if args.element != "none" and args.workload not in ("throw", "rapid-five", "speed-rapid-five"):
        parser.error("Elemental amulets require a throw workload")
    if args.require_volleys and args.workload not in ("turbo-B", "turbo-C"):
        parser.error("Volley observations require turbo-B or turbo-C")
    if any(not math.isfinite(value) or value <= 0 for value in
           (args.max_motion_ms, args.max_throw_ms, args.max_send_ms, args.max_release_ms,
            args.max_turbo_ms)):
        parser.error("Latency budget must be positive")
    try:
        directory = args.build_dir.resolve()
        if args.build:
            devenv.run(["cmake", "--build", str(directory), "--target", "netplayprobe", "-j", "2"])
        executable = directory / "bin" / ("netplayprobe.exe" if devenv.WINDOWS else "netplayprobe")
        if not executable.is_file():
            raise RuntimeError("netplayprobe is missing; build the target first")
        output = pathlib.Path(tempfile.mkdtemp(prefix="latency-", dir=directory))
        print(f"Reports: {output}", flush=True)
        failures = []
        settings = ("on", "off") if args.prediction == "both" and args.role == "pair" else (args.prediction if args.prediction != "both" else "on",)
        for fps in args.fps:
            for prediction in settings:
                run = output / f"{fps}fps-{prediction}"
                run.mkdir()
                with contextlib.ExitStack() as stack:
                    invitation = args.invitation.resolve() if args.invitation else run / "invitation.txt"
                    if args.role == "pair":
                        stack.callback(invitation.unlink, missing_ok=True)
                        host, host_report = launch(stack, executable, "host", invitation, run, args, fps, prediction)
                        wait_invitation(host, invitation)
                        guest, guest_report = launch(stack, executable, "guest", invitation, run, args, fps, prediction)
                        report = read_report(guest, guest_report, 180 + args.seconds)
                        authority = read_report(host, host_report, 20)
                        if (authority.get("workload", "throw") != args.workload or
                                authority.get("element", "none") != args.element or
                                authority.get("character", 0) != args.character or
                                authority.get("require_volleys", False) is not args.require_volleys):
                            failures.append(f"{run.name}: Host report loadout differs from requested configuration")
                    else:
                        process, path = launch(stack, executable, args.role, invitation, run, args, fps, prediction)
                        if args.role == "host":
                            wait_invitation(process, invitation)
                            print(f"Privately transfer {invitation} to guests. Do not publish it.", flush=True)
                        report = read_report(process, path, 180 + args.seconds)
                    if report["role"] == "guest":
                        budget = args.max_motion_ms if prediction == "on" else None
                        release_budget = args.max_release_ms if prediction == "on" else None
                        reasons = evaluate(report, budget, args.max_throw_ms, args.max_send_ms,
                                           release_budget, args.max_turbo_ms)
                        if (report.get("workload", "throw") != args.workload or
                                report.get("element", "none") != args.element or
                                report.get("character", 0) != args.character or
                                report.get("require_volleys", False) is not args.require_volleys):
                            reasons.append("Guest report loadout differs from requested configuration")
                        failures.extend(f"{run.name}: {reason}" for reason in reasons)
                        print(json.dumps({"run": run.name, "rtt": report["rtt"],
                                          "workload": args.workload,
                                          "character": args.character,
                                          "require_volleys": args.require_volleys,
                                          "volley_observations": {
                                              key: report.get(key) for key in
                                              ("displayed_volley_projectiles", "host_volley_projectiles",
                                               "matched_volley_projectiles", "displayed_volley_overflow",
                                               "host_volley_overflow")},
                                          "element": report.get("element", "none"),
                                          "element_observations": {
                                              key: report.get(key) for key in
                                              ("element_certified_observations", "displayed_element_projectiles",
                                               "host_element_projectiles", "displayed_invalid_element_projectiles",
                                               "host_invalid_element_projectiles")},
                                          "timing_scope": report.get("timing_scope", "visual-submit-no-audio"),
                                          "speed_observations": {
                                              prefix + suffix: report.get(prefix + suffix)
                                              for prefix in ("displayed_", "host_")
                                              for suffix in ("speed_certified_observations",
                                                             "speed_motion_observations",
                                                             "speed_minimum_pace", "speed_maximum_pace")},
                                          "turbo_startup": report.get("turbo_to_first_frame", {}),
                                          "turbo_host_action": report.get("turbo_to_host_action", {}),
                                          "turbo_observation_failures": {
                                              key: report.get(key) for key in
                                              ("pending_turbo_requests", "expired_turbo_requests",
                                               "interrupted_turbo_requests", "omitted_turbo_requests")},
                                          "motion": report.get("motion_to_first_frame", {}),
                                          "throw_startup": report.get("throw_to_first_frame", {}),
                                          "throw_authority": report.get("throw_to_authority", {}),
                                          "release_to_first_projectile_frame": report.get("release_to_first_projectile_frame", {}),
                                          "release_to_host_projectile": report.get("release_to_host_projectile", {}),
                                          "projectile_observation_failures": {
                                              key: report.get(key) for key in
                                              ("pending_releases", "expired_releases", "interrupted_releases",
                                               "unobserved_host_projectiles")},
                                          "send_queue": report["sample_to_send"],
                                          "resolved_deadline": report["sample_to_resolved"],
                                          "relayed": report.get("relayed"), "failures": reasons}), flush=True)
        if failures:
            raise RuntimeError("; ".join(failures))
        print("Measured runs completed. Same-machine results do not establish Internet performance.")
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Latency check failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
