#!/usr/bin/env python3
"""Private Compose entrypoint: impair eth0, measure native gameplay, retain evidence.

No host network, Docker socket, GPU, desktop input, assets in images, or public
relay is needed. NET_ADMIN applies only to this container's network namespace.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time

from netplay_netem import interface_commands, owns_root, safe_initial


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def run(command):
    return subprocess.run(command, check=True, capture_output=True, text=True, timeout=10)


def inspect(command):
    return json.loads(run(command).stdout)


def interrupted(*_):
    raise KeyboardInterrupt


def settings():
    delay, jitter, loss = (float(os.environ.get('GDL_PROBE_' + key, '0'))
                           for key in ('DELAY', 'JITTER', 'LOSS'))
    # Common validation, including finite/range checks.
    interface_commands('eth0', delay, jitter, loss)
    return {'delay_ms_each_way': delay, 'jitter_ms_each_way': jitter, 'loss_percent': loss}


def wait_invitation(path):
    until = time.monotonic() + 180
    while time.monotonic() < until:
        if path.is_file() and 6 < path.stat().st_size <= 2048:
            if path.read_text(encoding='utf-8').strip().startswith('gdl1-'):
                return
        time.sleep(0.05)
    raise RuntimeError('Host did not create an invitation; inspect its log')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('role', choices=('host', 'guest', 'check'))
    args = parser.parse_args(argv)
    if sys.platform != 'linux' or not Path('/.dockerenv').is_file():
        raise RuntimeError('This entrypoint runs only in the dedicated Docker container')
    configured = settings()
    show, add, remove = interface_commands('eth0', *configured.values())
    evidence = {'configuration': configured, 'before': inspect(show)}
    if not safe_initial(evidence['before']):
        raise RuntimeError('Refusing to replace an existing root queue discipline')
    run(add)  # Fails explicitly if sch_netem or NET_ADMIN is unavailable.
    evidence['configured'] = inspect(show)
    if not owns_root(evidence['configured']):
        raise RuntimeError('netem was not attached to eth0')
    participant = args.role + '-' + socket.gethostname()
    output = Path('/results')
    process = None
    try:
        if args.role == 'check':
            print('Linux netem is available in the isolated container', flush=True)
            return 0
        report = output / (participant + '.json')
        invitation = output / 'invitation.txt'
        if args.role == 'guest':
            wait_invitation(invitation)
        command = ['setpriv', '--bounding-set=-all', '--inh-caps=-all',
                   '--ambient-caps=-all', '--no-new-privs', '/opt/probe/netplayprobe',
                   args.role, str(invitation), str(report), '--assets', '/assets',
                   '--data', '/opt/probe/data', '--hold-after-report']
        for flag, key, default in (('seconds', 'SECONDS', '20'), ('players', 'PLAYERS', '2'),
                                   ('fps', 'FPS', '60'), ('prediction', 'PREDICTION', 'on')):
            command.extend(['--' + flag, os.environ.get('GDL_PROBE_' + key, default)])
        with report.with_suffix('.log').open('w', encoding='utf-8') as log:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT)
            try:
                code = process.wait(timeout=220 + int(os.environ.get('GDL_PROBE_SECONDS', '20')))
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
        evidence['exit_code'] = code
        return code
    finally:
        evidence['after'] = inspect(show)
        if args.role != 'check':
            write_json(output / (participant + '.network.json'), evidence)
        if owns_root(evidence['after']):
            run(remove)


if __name__ == '__main__':
    # Raise through the cleanup path when Compose stops this wrapper.
    signal.signal(signal.SIGTERM, interrupted)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'Container probe failed: {error}', file=sys.stderr)
        sys.exit(1)
