#!/usr/bin/env python3
"""Run native host/guests over real iroh in isolated Linux Docker containers.

  python scripts/netplay_docker.py --check
  python scripts/netplay_docker.py --build --fps 30 60
  python scripts/netplay_docker.py --players 3 --profiles baseline regional

Requires Docker Desktop (Linux containers) or Docker Engine + Compose. The Linux
kernel must support sch_netem. The --check probe never restarts Docker or WSL.
Place Docker's disk image/cache on a drive with ample space before --build; the
first C++/Rust/dependency build can occupy several GB. Reports default to build/.
Disc assets are mounted read-only, never sent in the build context or image.

These are measured application responses over kernel-impaired real packets, NOT
physical input-to-photon or geographically remote Internet measurements. Public
relays are disabled. Each endpoint applies its delay to egress (RTT adds twice
that delay). Latency budget failures retain measurements and fail the command;
they do not stop the remaining A/B cases. No host networking or published ports.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

import devenv
from netplay_latency import evaluate
from netplay_netem import owns_root

ROOT = devenv.ROOT
DOCKERFILE = ROOT / 'scripts/netplay/Dockerfile'
COMPOSE = ROOT / 'scripts/netplay/compose.yaml'
IMAGE = 'gdl-netplay-probe:local'
# Egress at BOTH endpoints, not an invented telemetry RTT.
PROFILES = {'baseline': (0, 0, 0), 'regional': (25, 5, 0), 'wan': (50, 10, 1)}


def execute(command, *, env=None, timeout=30):
    return subprocess.run(command, cwd=ROOT, env=env, check=True, text=True,
                          capture_output=True, timeout=timeout,
                          creationflags=subprocess.CREATE_NO_WINDOW if devenv.WINDOWS else 0)


def docker_check():
    try:
        info = json.loads(execute(['docker', 'info', '--format', '{{json .}}'], timeout=20).stdout)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError('Docker engine did not answer within 20 seconds. Restart/check Docker '
                           'Desktop; no containers or network settings were changed.') from error
    if info.get('OSType') != 'linux':
        raise RuntimeError('Select Linux containers in Docker Desktop')
    compose = execute(['docker', 'compose', 'version', '--short']).stdout.strip()
    return {'engine': info.get('ServerVersion'), 'kernel': info.get('KernelVersion'),
            'architecture': info.get('Architecture'), 'compose': compose}


def source_digest():
    """Attest working files, not just HEAD: dirty source must rebuild the image."""
    paths = [ROOT / name for name in ('CMakeLists.txt', 'CMakePresets.json', 'VERSION', 'vcpkg.json')]
    for directory in ('src', 'cmake', 'tests', 'tools', 'shaders', 'data'):
        for base, dirs, files in os.walk(ROOT / directory):
            dirs[:] = sorted(d for d in dirs if d not in ('target', '__pycache__', '.git'))
            paths.extend(Path(base) / name for name in files if not name.endswith('.md'))
    paths.extend([DOCKERFILE, DOCKERFILE.with_name('Dockerfile.dockerignore'),
                  ROOT / 'scripts/netplay/container.py', ROOT / 'scripts/netplay/build.py',
                  ROOT / 'scripts/netplay_netem.py'])
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(path.relative_to(ROOT).as_posix().encode() + b'\0')
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def network_failures(report, evidence, delay, jitter, loss):
    """Reject bypassed impairment even when predicted motion looks instantaneous."""
    problems = []
    expected = evidence.get('configuration', {})
    if (expected.get('delay_ms_each_way') != delay or expected.get('jitter_ms_each_way') != jitter
            or expected.get('loss_percent') != loss):
        problems.append('Missing/mismatched network configuration evidence')
    for phase in ('configured', 'after'):
        if not owns_root(evidence.get(phase, [])):
            problems.append(f'Owned netem queue missing: {phase}')
    rows = [row for row in evidence.get('after', [])
            if row.get('root') and row.get('kind') == 'netem' and row.get('handle') == '7d1:']
    if not rows or rows[0].get('packets', 0) <= 0:
        problems.append('No real packets observed through netem')
    if evidence.get('exit_code') != 0:
        problems.append('Native probe did not exit successfully')
    if report.get('role') == 'guest':
        rtt = report.get('rtt', {})
        measured = rtt.get('median_ms')
        if not rtt.get('count') or not isinstance(measured, (int, float)) or not math.isfinite(measured):
            problems.append('No measured network RTT')
        elif measured < max(0, 2 * (delay - jitter) * 0.75):
            problems.append('Measured RTT is below the impaired path; delay may have been bypassed')
        if report.get('relayed') is not False:
            problems.append('Expected a verified direct path on the isolated Docker network')
    return problems


def compose_command(project, *arguments):
    # Never accepts an existing/user project name; caller generates one per run.
    return ['docker', 'compose', '-f', str(COMPOSE), '-p', project, *arguments]


def run_case(directory, args, profile, fps, prediction):
    delay, jitter, loss = PROFILES[profile]
    project = 'gdl-latency-' + uuid.uuid4().hex[:12]
    environment = dict(os.environ, GDL_PROBE_IMAGE=IMAGE,
                       GDL_PROBE_ASSETS=str(args.assets.resolve()), GDL_PROBE_OUTPUT=str(directory),
                       GDL_PROBE_DELAY=str(delay), GDL_PROBE_JITTER=str(jitter),
                       GDL_PROBE_LOSS=str(loss), GDL_PROBE_SECONDS=str(args.seconds),
                       GDL_PROBE_PLAYERS=str(args.players), GDL_PROBE_FPS=str(fps),
                       GDL_PROBE_PREDICTION=prediction)
    result = {'profile': profile, 'fps': fps, 'prediction': prediction,
              'players': args.players, 'delay_ms_each_way': delay,
              'jitter_ms_each_way': jitter, 'loss_percent': loss, 'guests': [], 'failures': []}
    def compose(*arguments, timeout=30):
        return execute(compose_command(project, *arguments), env=environment, timeout=timeout)
    try:
        compose('run', '--rm', '--no-deps', 'host', 'check', timeout=30)
        compose('up', '-d', '--no-build', '--pull', 'never', '--scale', f'guest={args.players - 1}', timeout=60)
        deadline = time.monotonic() + 240 + args.seconds
        while time.monotonic() < deadline:
            # Network evidence is written after the native process exits, not
            # while its report might still be partially written.
            if len(list(directory.glob('*.network.json'))) == args.players:
                break
            failed = compose('ps', '--all', '--format', 'json').stdout.strip()
            if failed:
                # Compose releases use either a JSON array or JSON lines.
                rows = json.loads(failed) if failed.startswith('[') else [json.loads(line) for line in failed.splitlines()]
                if any(row.get('State') in ('exited', 'dead') and row.get('ExitCode', 0) != 0 for row in rows):
                    raise RuntimeError('A container failed; inspect the retained logs')
            time.sleep(1)
        else:
            raise RuntimeError('Container measurement timed out')
        reports = list(directory.glob('host-*.network.json')) + list(directory.glob('guest-*.network.json'))
        if len(reports) != args.players or len(list(directory.glob('host-*.network.json'))) != 1:
            raise RuntimeError('Missing or unexpected participant evidence')
        for path in reports:
            evidence = json.loads(path.read_text(encoding='utf-8'))
            report = json.loads(path.with_name(path.name.replace('.network.json', '.json')).read_text(encoding='utf-8'))
            problems = network_failures(report, evidence, delay, jitter, loss)
            if report.get('status') != 'complete':
                problems.append('Incomplete native measurement')
            if report.get('role') == 'guest':
                problems.extend(evaluate(report, args.max_motion_ms if prediction == 'on' else None,
                                         args.max_throw_ms, args.max_send_ms))
                result['guests'].append({'report': path.name.replace('.network.json', '.json'),
                                         'rtt': report.get('rtt'),
                                         'movement': report.get('motion_to_first_frame'),
                                         'throw': report.get('throw_to_first_frame'),
                                         'send_queue': report.get('sample_to_send'),
                                         'confirmation': report.get('sample_to_resolved'),
                                         'input_lead_ticks': report.get('input_lead_ticks'),
                                         'failures': problems})
            result['failures'].extend(path.stem + ': ' + problem for problem in problems)
        if len(result['guests']) != args.players - 1:
            result['failures'].append('Not every guest supplied a report')
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result['failures'].append(str(error))
    finally:
        try:
            logs = compose('logs', '--no-color').stdout
            # Probe logs contain paths and diagnostics, never ticket contents.
            (directory / 'containers.log').write_text(logs, encoding='utf-8')
        except (OSError, subprocess.SubprocessError) as error:
            result['failures'].append(f'Could not collect container diagnostics: {error}')
        try:
            compose('down', '--remove-orphans', '--timeout', '5', timeout=30)
        except (OSError, subprocess.SubprocessError) as error:
            result['failures'].append(f'Cleanup failed: {error}. Retry docker compose -f {COMPOSE} -p {project} down')
        (directory / 'invitation.txt').unlink(missing_ok=True)
    (directory / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', action='store_true', help='Check engine/Compose only; no mutation')
    parser.add_argument('--build', action='store_true', help='Build/update the asset-free Linux probe image')
    parser.add_argument('--assets', type=Path, default=ROOT / 'assets/GUNE5D/Gauntlet')
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/netplay-docker')
    parser.add_argument('--profiles', choices=tuple(PROFILES), nargs='+', default=list(PROFILES))
    parser.add_argument('--fps', type=int, choices=(30, 60), nargs='+', default=[60])
    parser.add_argument('--players', type=int, choices=(2, 3, 4), default=2)
    parser.add_argument('--seconds', type=int, choices=range(8, 301), metavar='8..300', default=20)
    parser.add_argument('--prediction', choices=('on', 'off', 'both'), default='both')
    parser.add_argument('--max-motion-ms', type=float, default=75)
    parser.add_argument('--max-throw-ms', type=float, default=200)
    parser.add_argument('--max-send-ms', type=float, default=10)
    args = parser.parse_args(argv)
    if any(not math.isfinite(value) or value <= 0 for value in
           (args.max_motion_ms, args.max_throw_ms, args.max_send_ms)):
        parser.error('Latency budgets must be finite and positive')
    try:
        if not shutil.which('docker'):
            raise RuntimeError('Docker CLI not found; install/start Docker Desktop or Docker Engine with Compose')
        engine = docker_check()
        print(json.dumps(engine), flush=True)
        if args.check:
            return 0
        if not (args.assets / 'WDATA').is_dir():
            raise RuntimeError('Native Gauntlet asset directory is missing; use --assets')
        args.output_root.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix='run-', dir=args.output_root.resolve()))
        print(f'Reports: {output}', flush=True)
        digest = source_digest()
        if args.build:
            with (output / 'build.log').open('w', encoding='utf-8') as log:
                subprocess.run(['docker', 'build', '--progress', 'plain', '-f', str(DOCKERFILE),
                                '--label', f'org.gdl.probe.source={digest}', '-t', IMAGE, str(ROOT)],
                               cwd=ROOT, check=True, stdout=log, stderr=subprocess.STDOUT,
                               creationflags=subprocess.CREATE_NO_WINDOW if devenv.WINDOWS else 0)
        image = json.loads(execute(['docker', 'image', 'inspect', IMAGE]).stdout)[0]
        if image.get('Config', {}).get('Labels', {}).get('org.gdl.probe.source') != digest:
            raise RuntimeError('Probe image is stale relative to the working files; rerun with --build')
        summary = {'engine': engine, 'image': image['Id'], 'source_sha256': digest,
                   'version': (ROOT / 'VERSION').read_text().strip(), 'runs': [],
                   'scope': 'Headless native gameplay; real iroh, kernel-emulated network; no GPU/scanout'}
        for profile in args.profiles:
            for fps in args.fps:
                for prediction in (('on', 'off') if args.prediction == 'both' else (args.prediction,)):
                    directory = output / f'{profile}-{fps}fps-{prediction}'
                    directory.mkdir()
                    # The root-owned, capability-free container can write its
                    # bind mount on Linux. The enclosing mkdtemp directory is
                    # private (0700), so other host users cannot traverse it.
                    directory.chmod(0o777)
                    print(f'Measuring {directory.name} ({args.players} players)', flush=True)
                    result = run_case(directory, args, profile, fps, prediction)
                    summary['runs'].append(result)
                    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
                    print(json.dumps(result), flush=True)
        return int(any(run['failures'] for run in summary['runs']))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'Docker latency check failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
