#!/usr/bin/env python3
"""Temporarily impair a dedicated Linux network namespace, never the host network.

Preview (no changes):
  python scripts/netplay_netem.py --namespace gdl-guest --interface eth0 --delay-ms 50 --jitter-ms 10 --loss 1
Apply while running a probe already routed through that namespace:
  sudo python scripts/netplay_netem.py --namespace gdl-guest --interface eth0 --delay-ms 50 --seconds 120 --apply

This does NOT create a namespace, VPN, route, or remote server. Supply an isolated
test namespace with working connectivity. netem delays EGRESS only; 50 ms at one
endpoint adds about 50 ms to RTT. Apply at both endpoints for symmetric delay.
Run the guest INSIDE the named namespace; otherwise the impairment is bypassed.
All selected iroh paths must traverse the impaired interface. Verify RTT/path in
the probe report. An Internet-enabled local test alone proves neither relay use
nor a geographically remote route.

Refuses existing root disciplines (except noqueue), uses an owned handle, and
cleans up on normal exit/Ctrl+C. SIGKILL/power loss requires manual cleanup of
the printed namespace-specific command. Never replaces an existing discipline.
"""
import argparse
import json
import math
import re
import shlex
import subprocess
import sys
import time


def interface_commands(interface, delay_ms, jitter_ms, loss):
    """Queue operations for a caller already inside an isolated network namespace."""
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,62}", interface):
        raise ValueError("Use a plain interface name")
    for value in (delay_ms, jitter_ms, loss):
        if not math.isfinite(value) or value < 0:
            raise ValueError("Impairment values must be finite and nonnegative")
    if delay_ms > 2000 or jitter_ms > delay_ms or loss > 20:
        raise ValueError("Require delay <= 2000 ms, jitter <= delay, and loss <= 20%")
    prefix = ["tc", "qdisc"]
    show = ["tc", "-s", "-j", "qdisc", "show", "dev", interface]
    add = prefix + ["add", "dev", interface, "root", "handle", "7d1:", "netem",
                    "delay", f"{delay_ms:g}ms", f"{jitter_ms:g}ms", "loss", f"{loss:g}%"]
    remove = prefix + ["del", "dev", interface, "root", "handle", "7d1:"]
    return show, add, remove


def commands(namespace, interface, delay_ms, jitter_ms, loss):
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,62}", namespace):
        raise ValueError("Use a plain namespace name")
    return tuple(["ip", "netns", "exec", namespace] + command
                 for command in interface_commands(interface, delay_ms, jitter_ms, loss))


def safe_initial(rows):
    return all(not row.get("root") or row.get("kind") == "noqueue" for row in rows)


def owns_root(rows):
    return any(row.get("root") and row.get("kind") == "netem" and row.get("handle") == "7d1:"
               for row in rows)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--namespace", required=True)
    parser.add_argument("--interface", required=True)
    parser.add_argument("--delay-ms", type=float, default=50)
    parser.add_argument("--jitter-ms", type=float, default=0)
    parser.add_argument("--loss", type=float, default=0)
    parser.add_argument("--seconds", type=int, choices=range(1, 3601), metavar="1..3600", default=120)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args(argv)
    try:
        show, add, remove = commands(args.namespace, args.interface, args.delay_ms, args.jitter_ms, args.loss)
        print("Apply:", shlex.join(add))
        print("Cleanup:", shlex.join(remove))
        if not args.apply:
            print("Preview only; no network settings changed.")
            return 0
        if sys.platform != "linux":
            raise ValueError("Apply requires Linux and an existing isolated network namespace")
        def inspect():
            return json.loads(subprocess.run(show, check=True, capture_output=True, text=True).stdout)
        if not safe_initial(inspect()):
            raise ValueError("Existing root queue discipline; refusing to replace it")
        subprocess.run(add, check=True)
        try:
            print("Impairment active in the named namespace only. Ctrl+C restores it.", flush=True)
            time.sleep(args.seconds)
        finally:
            if owns_root(inspect()):
                subprocess.run(remove, check=True)
            else:
                print("Owned discipline no longer present; leaving replacement untouched.", file=sys.stderr)
        return 0
    except KeyboardInterrupt:
        return 130
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"Network impairment failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
