import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import netplay_docker as harness
from netplay_netem import commands, interface_commands

spec = importlib.util.spec_from_file_location('netplay_container', ROOT / 'scripts/netplay/container.py')
container = importlib.util.module_from_spec(spec)
spec.loader.exec_module(container)


class DockerLatencyTests(unittest.TestCase):
    def evidence(self):
        queue = {'root': True, 'kind': 'netem', 'handle': '7d1:', 'packets': 500}
        return {'configuration': {'delay_ms_each_way': 25, 'jitter_ms_each_way': 5, 'loss_percent': 0},
                'configured': [queue.copy()], 'after': [queue.copy()], 'exit_code': 0}

    def report(self):
        return {'role': 'guest', 'relayed': False, 'rtt': {'count': 20, 'median_ms': 50}}

    def test_measured_rtt_and_transmitted_packets_are_required(self):
        report, evidence = self.report(), self.evidence()
        self.assertEqual(harness.network_failures(report, evidence, 25, 5, 0), [])
        report['rtt']['median_ms'] = 1
        self.assertTrue(harness.network_failures(report, evidence, 25, 5, 0))
        report = self.report()
        for count in (0, None):
            evidence['after'][0]['packets'] = count or 0
            self.assertTrue(harness.network_failures(report, evidence, 25, 5, 0))

    def test_absent_replaced_or_wrong_queue_cannot_pass(self):
        for phase in ('configured', 'after'):
            for key, value in (('kind', 'fq_codel'), ('handle', '10:'), ('root', False)):
                evidence = self.evidence()
                evidence[phase][0][key] = value
                self.assertTrue(harness.network_failures(self.report(), evidence, 25, 5, 0))
        self.assertTrue(harness.network_failures(self.report(), {}, 25, 5, 0))

    def test_failed_process_or_mismatched_profile_cannot_pass(self):
        for key in ('delay_ms_each_way', 'jitter_ms_each_way', 'loss_percent'):
            evidence = self.evidence()
            evidence['configuration'][key] += 1
            self.assertTrue(harness.network_failures(self.report(), evidence, 25, 5, 0))
        evidence = self.evidence()
        evidence['exit_code'] = 1
        self.assertTrue(harness.network_failures(self.report(), evidence, 25, 5, 0))

    def test_unobserved_or_relay_path_is_not_direct_path_proof(self):
        for report in ({}, {'role': 'guest'}, dict(self.report(), relayed=True)):
            self.assertTrue(harness.network_failures(report, {}, 25, 5, 0))
        for rtt in ({}, {'count': 0, 'median_ms': 50}, {'count': 5, 'median_ms': float('nan')}):
            report = self.report()
            report['rtt'] = rtt
            self.assertTrue(harness.network_failures(report, self.evidence(), 25, 5, 0))

    def test_existing_namespace_helper_keeps_its_scoping(self):
        direct = interface_commands('eth0', 25, 5, 1)
        named = commands('probe', 'eth0', 25, 5, 1)
        for bare, scoped in zip(direct, named):
            self.assertEqual(scoped, ['ip', 'netns', 'exec', 'probe'] + bare)
        self.assertNotIn('replace', direct[1])
        self.assertIn('-s', direct[0])

    @patch('netplay_docker.execute', side_effect=subprocess.TimeoutExpired('docker', 20))
    def test_engine_timeout_is_bounded_and_does_not_restart_it(self, execute):
        with self.assertRaisesRegex(RuntimeError, 'did not answer'):
            harness.docker_check()
        self.assertEqual(execute.call_count, 1)
        self.assertEqual(execute.call_args.kwargs['timeout'], 20)

    @patch('netplay_docker.execute')
    def test_requires_linux_engine_and_compose(self, execute):
        execute.return_value = SimpleNamespace(stdout='{"OSType":"windows"}')
        with self.assertRaisesRegex(RuntimeError, 'Linux containers'):
            harness.docker_check()
        execute.side_effect = [SimpleNamespace(stdout='{"OSType":"linux","KernelVersion":"test"}'),
                               SimpleNamespace(stdout='2.40')]
        self.assertEqual(harness.docker_check()['compose'], '2.40')

    @patch('netplay_docker.execute', side_effect=subprocess.CalledProcessError(1, 'compose'))
    def test_failed_start_still_cleans_only_its_project_and_private_invitation(self, execute):
        args = SimpleNamespace(assets=ROOT / 'assets/GUNE5D/Gauntlet', seconds=8, players=2)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'invitation.txt').write_text('private ticket', encoding='utf-8')
            result = harness.run_case(directory, args, 'baseline', 60, 'on')
            self.assertTrue(result['failures'])
            self.assertFalse((directory / 'invitation.txt').exists())
            self.assertTrue((directory / 'summary.json').is_file())
        calls = [call.args[0] for call in execute.call_args_list]
        self.assertIn('down', calls[-1])
        self.assertTrue(all(call[5].startswith('gdl-latency-') for call in calls))
        self.assertEqual(len({call[5] for call in calls}), 1)
        self.assertFalse(any('prune' in call for call in calls))

    @patch.dict('os.environ', {'GDL_PROBE_DELAY': '25', 'GDL_PROBE_JITTER': '5', 'GDL_PROBE_LOSS': '1'})
    def test_container_reads_and_validates_same_profile(self):
        self.assertEqual(container.settings(), {'delay_ms_each_way': 25, 'jitter_ms_each_way': 5,
                                               'loss_percent': 1})
        with patch.dict('os.environ', {'GDL_PROBE_DELAY': 'nan'}):
            with self.assertRaises(ValueError):
                container.settings()

    def test_compose_does_not_expose_host_network_socket_or_writeable_assets(self):
        text = harness.COMPOSE.read_text(encoding='utf-8')
        self.assertIn('internal: true', text)
        self.assertIn('cap_drop: [ALL]', text)
        self.assertIn('create_host_path: false', text)
        self.assertNotIn('privileged:', text)
        self.assertNotIn('network_mode:', text)
        self.assertNotIn('ports:', text)
        self.assertNotIn('docker.sock', text)
        assets = text[text.index('source: ${GDL_PROBE_ASSETS'):text.index('source: ${GDL_PROBE_OUTPUT')]
        self.assertIn('read_only: true', assets)

    def test_context_is_an_allowlist_without_assets_or_documentation(self):
        lines = (ROOT / 'scripts/netplay/Dockerfile.dockerignore').read_text().splitlines()
        self.assertEqual(next(line for line in lines if line and not line.startswith('#')), '**')
        self.assertFalse(any(line.startswith('!assets') or line.startswith('!.git') for line in lines))
        self.assertIn('**/*.md', lines)
        self.assertIn('**/target/**', lines)

    @patch('netplay_docker.execute')
    def test_every_guest_is_evaluated_and_slow_attacks_are_retained(self, execute):
        args = SimpleNamespace(assets=ROOT / 'assets/GUNE5D/Gauntlet', seconds=20, players=3,
                               max_motion_ms=75, max_throw_ms=200, max_send_ms=10)
        execute.return_value = SimpleNamespace(stdout='')
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for name in ('host-a', 'guest-b', 'guest-c'):
                report = dict(self.report(), schema=3, workload='throw', status='complete', render_fps=60,
                              missed_motion=0, missed_throws=0,
                              motion_to_first_frame={'count': 10, 'p95_ms': 25},
                              throw_to_first_frame={'count': 10, 'p95_ms': 250 if name == 'guest-c' else 150},
                              throw_to_authority={'count': 10, 'p95_ms': 125},
                              pending_throw_authority=0, expired_throw_authority=0,
                              interrupted_throw_authority=0, superseded_throw_authority=0,
                              release_to_first_projectile_frame={'count': 10, 'p95_ms': 25},
                              release_to_host_projectile={'count': 10, 'p95_ms': 100},
                              pending_releases=0, expired_releases=0, interrupted_releases=0,
                              unobserved_host_projectiles=0,
                              sample_to_send={'count': 500, 'p95_ms': 1},
                              sample_to_resolved={'count': 500},
                              render_interval={'count': 500, 'median_ms': 16.67},
                              update_interval={'count': 500, 'median_ms': 16.67})
                if name.startswith('host'):
                    report['role'] = 'host'
                (directory / (name + '.json')).write_text(json.dumps(report))
                (directory / (name + '.network.json')).write_text(json.dumps(self.evidence()))
            result = harness.run_case(directory, args, 'regional', 60, 'on')
            self.assertEqual(len(result['guests']), 2)
            self.assertEqual(len(result['failures']), 1)
            self.assertIn('throw', result['failures'][0])
            self.assertTrue(result['failures'][0].startswith('guest-c.network:'))
            self.assertEqual(len(list(directory.glob('guest-*.json'))), 4)
            self.assertEqual(json.loads((directory / 'summary.json').read_text()), result)
            # Fast animation alone is insufficient even through the Docker
            # wrapper: missing projectile evidence must remain a guest failure.
            guest = directory / 'guest-b.json'
            report = json.loads(guest.read_text())
            del report['release_to_first_projectile_frame']
            guest.write_text(json.dumps(report))
            result = harness.run_case(directory, args, 'regional', 60, 'on')
            self.assertIn('guest-b.network: No samples for release_to_first_projectile_frame',
                          result['failures'])
            self.assertTrue(any(failure.startswith('guest-c.network:')
                                for failure in result['failures']))
        self.assertIn('guest=2', execute.call_args_list[1].args[0])

    @patch('netplay_docker.source_digest')
    @patch('netplay_docker.docker_check', return_value={'engine': 'test'})
    @patch('netplay_docker.shutil.which', return_value='docker')
    def test_check_never_builds_or_requires_assets(self, which, check, digest):
        self.assertEqual(harness.main(['--check', '--assets', '/missing']), 0)
        check.assert_called_once()
        digest.assert_not_called()


if __name__ == '__main__':
    unittest.main()
