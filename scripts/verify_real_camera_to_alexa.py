#!/usr/bin/env python3
"""Independent JPEG acquisition -> native analysis -> SQLite -> MCP -> ASK check.

The camera peer and all images are generated fixtures, never real captures.
Requires FFmpeg and the existing ASK Python dependencies. No AWS calls or deployment.
"""
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import argparse
import base64
import importlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

from verify_scenarios_to_alexa import (
    Fixture, INTENTS, ROOT, RANK, clean_env, envelope, free_port,
    private_file, require, sdk_environment, speech_text,
)
import urllib.error
import urllib.request


WIDTH, HEIGHT = 160, 120


def jpeg_fixtures():
    """Quiet baseline, a settled static scene, then sustained movement.

    The shape matters: the rule correlates evidence inside a wall-clock window, so
    the movement has to be fed at the acquisition cadence. No threshold is changed.
    """
    planes = []
    for index in range(40):
        plane = bytearray([32] * (WIDTH * HEIGHT))
        if index >= 12:
            x = 60 if index < 30 else 10 + (index - 30) * 13
            for y in range(35, 75):
                plane[y * WIDTH + x:y * WIDTH + x + 44] = bytes([220] * 44)
        planes.append(bytes(plane))
    result = subprocess.run([
        '/usr/bin/ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error',
        '-f', 'rawvideo', '-pixel_format', 'gray', '-video_size', '160x120',
        '-i', 'pipe:0', '-frames:v', str(len(planes)), '-c:v', 'mjpeg',
        '-threads', '1', '-q:v', '2', '-f', 'image2pipe', 'pipe:1',
    ], input=b''.join(planes), capture_output=True, timeout=20)
    require(result.returncode == 0, 'synthetic JPEG fixture encoding failed')
    frames = []
    start = 0
    while start < len(result.stdout):
        require(result.stdout[start:start + 2] == b'\xff\xd8', 'fixture JPEG SOI missing')
        end = result.stdout.find(b'\xff\xd9', start + 2)
        require(end >= 0, 'fixture JPEG EOI missing')
        frames.append(result.stdout[start:end + 2])
        start = end + 2
    require(len(frames) == len(planes), 'fixture frame count mismatch')
    require(all(len(frame) < 512 * 1024 for frame in frames), 'fixture exceeds source cap')
    return frames


@contextmanager
def camera_peer(password, frames):
    class State:
        calls = 0
        index = 0
        malformed = False
    state = State()
    expected = 'Basic ' + base64.b64encode(('admin:' + password).encode()).decode()

    class Peer(BaseHTTPRequestHandler):
        def log_message(self, format, *args):
            pass

        def do_GET(self):
            state.calls += 1
            if self.path != '/snapshot' or self.headers.get('Authorization') != expected:
                self.send_response(401)
                self.end_headers()
                return
            body = b'not-a-jpeg' if state.malformed else frames[min(state.index, len(frames) - 1)]
            if not state.malformed:
                state.index += 1
            self.send_response(200)
            self.send_header('Content-Type', 'image/jpeg')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Peer)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield server.server_port, state
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


class CameraFixture(Fixture):
    def __init__(self, directory, build, peer_port, password):
        super().__init__(directory, build, True)
        self.registry = directory / 'camera-registry.json'
        self.device = {
            'device_id': 'camera-syn-001', 'device_type': 'simulator',
            'display_name': 'Generated JPEG fixture', 'credential_id': 'a' * 64,
            'paired': True, 'revoked': False, 'has_camera': True,
            'has_microphone': False, 'capabilities': ['camera', 'motion'],
            'consent_granted': True, 'consent_subject': 'synthetic-fixture',
            'consent_purpose': 'home_security', 'capture_enabled': True,
        }
        self.write_registry()
        self.password_file = directory / 'camera.password'
        private_file(self.password_file, password)
        self.peer_port = peer_port
        self.env['HG_BRIDGE_AUDIT_DB'] = self.env['HG_MCP_AUDIT_DB']

    def write_registry(self, **changes):
        record = dict(self.device, **changes)
        self.registry.write_text(json.dumps({'devices': [record]}))
        self.registry.chmod(0o600)

    def writer(self):
        port = free_port()
        self.base = 'http://127.0.0.1:' + str(port)
        return self.start('camera-writer', [
            str(self.build / 'hg_alexa_bridge'), '--http', str(self.config),
            str(self.credentials), '--port', str(port), '--secret-file', str(self.secret),
            '--uploads-dir', str(self.directory / 'uploads'), '--camera-host', '127.0.0.1',
            '--camera-port', str(self.peer_port), '--camera-password-file', str(self.password_file),
            '--camera-allow-plaintext', '--camera-analysis', '--camera-registry', str(self.registry),
            '--camera-device-id', 'camera-syn-001', '--camera-credential-id', 'a' * 64,
        ], port)

    def camera_request(self, path, authorized=True):
        headers = {'Origin': self.base}
        if authorized:
            headers['X-HG-Secret'] = self.secret.read_text()
        request = urllib.request.Request(self.base + path, headers=headers)
        try:
            with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=10) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as error:
            try:
                return error.code, error.read()
            finally:
                error.close()

    def analysis(self):
        code, body = self.camera_request('/api/camera/status')
        require(code == 200, 'authorized analysis status unavailable')
        value = json.loads(body)
        require(isinstance(value.get('analysis'), dict), 'camera analysis is not connected')
        return value['analysis']


def run(build, report):
    frames = jpeg_fixtures()
    with sdk_environment(clean_env()):
        sys.path.insert(0, str(ROOT / 'alexa_skill_python/lambda'))
        backend = importlib.import_module('backend')
        skill = importlib.import_module('lambda_function')
    scratch = os.environ.get('TMPDIR')
    require(bool(scratch), 'TMPDIR is required')
    with tempfile.TemporaryDirectory(prefix='hg-camera-alexa-', dir=scratch) as temp:
        root = Path(temp)
        with camera_peer('generated-fixture-password', frames) as (peer_port, peer):
            fixture = CameraFixture(root, build, peer_port, 'generated-fixture-password')
            try:
                writer = fixture.writer()
                require(fixture.analysis().get('enabled') is True, 'analysis opt-in was not enabled')
                require(fixture.camera_request('/api/camera/frame', False)[0] == 401,
                        'camera acquisition did not require operator authorization')
                require(peer.calls == 0, 'unauthorized request reached the camera')
                report['negatives'] = []
                for label, changes in [
                    ('capture_disabled', {'capture_enabled': False}),
                    ('consent_withdrawn', {'consent_granted': False}),
                    ('revoked', {'revoked': True}),
                    ('unpaired', {'paired': False}),
                    ('credential_mismatch', {'credential_id': 'b' * 64}),
                ]:
                    fixture.write_registry(**changes)
                    code, _ = fixture.camera_request('/api/camera/frame?t=refused')
                    require(code in (401, 403, 503), label + ' did not refuse acquisition')
                    require(peer.calls == 0, label + ' still fetched a frame')
                    report['negatives'].append(label)
                fixture.write_registry()
                trace = []
                for index in range(len(frames)):
                    code, body = fixture.camera_request('/api/camera/frame?t=' + str(index))
                    require(code == 200 and body[:2] == b'\xff\xd8',
                            'authorized fixture frame failed (code %d)' % code)
                    state = fixture.analysis()
                    observation = state.get('last_observation')
                    require(isinstance(observation, dict), 'decoded frame did not produce an observation')
                    require(observation.get('provenance') == 'synthetic', 'fixture provenance was upgraded')
                    require(observation.get('observation_id'), 'observation ID missing')
                    trace.append(observation)
                    if index == 11:
                        require(state.get('alerts_committed') == 0, 'quiet baseline produced an alert')
                    time.sleep(.60 if index >= 30 else .25)
                state = fixture.analysis()
                require(state.get('observations_processed') == len(frames), 'a fetched frame escaped analysis')
                require(peer.calls == len(frames), 'unexpected camera request count')
                require(state.get('alerts_committed', 0) > 0, 'sustained motion produced no durable alert')
                fixture.stop(writer)
                db = Path(fixture.env['HG_MCP_AUDIT_DB'])
                with sqlite3.connect(db.as_uri() + '?mode=ro', uri=True) as connection:
                    rows = connection.execute(
                        'SELECT alert_id,severity,contributing_event_ids,details FROM alerts ORDER BY alert_id').fetchall()
                require(len(rows) == state['alerts_committed'], 'commit counter differs from durable store')
                observed_ids = {observation['observation_id'] for observation in trace}
                require(len(observed_ids) == len(frames), 'duplicate acquisition observation IDs')
                for row in rows:
                    contributing = json.loads(row[2])
                    details = json.loads(row[3])
                    require(bool(contributing) and set(contributing) <= observed_ids,
                            'committed alert references unobserved or fabricated evidence')
                    require(details.get('provenance') == 'synthetic', 'fixture alert was labelled physical')
                    require(details.get('provenance_derived') is True, 'alert provenance was not derived')
                ids = [row[0] for row in rows]
                highest = max((row[1] for row in rows), key=lambda severity: RANK[severity])
                require(highest != 'critical', 'motion was incorrectly promoted to a safety incident')
                fixture.reader()
                with sdk_environment(fixture.env):
                    summary = backend.get_alert_summary('synthetic-verifier-user')
                    require(isinstance(summary, dict), 'native MCP summary unavailable')
                    require(summary.get('alert_count') == len(rows), 'MCP count mismatch')
                    require(set(summary.get('alert_ids', [])) == set(ids), 'MCP alert IDs mismatch')
                    require(summary.get('highest_severity') == highest, 'MCP severity mismatch')
                    expected = 'There %s %d alert%s recorded. The highest severity is %s.' % (
                        'is' if len(rows) == 1 else 'are', len(rows), '' if len(rows) == 1 else 's', highest)
                    speeches = {}
                    for intent in INTENTS:
                        speeches[intent] = speech_text(skill.lambda_handler(envelope(intent), None))
                        require(speeches[intent] == expected, intent + ' readback mismatch')
                report.update(passed=True, frames=len(frames), trace=trace, analysis=state,
                              sqlite_alerts=[{'alert_id': r[0], 'severity': r[1],
                                              'contributing_event_ids': json.loads(r[2]),
                                              'details': json.loads(r[3])} for r in rows],
                              mcp_summary=summary, ask_speech=speeches)
            finally:
                fixture.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'hg_build')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = {'passed': False, 'scope': 'generated JPEG peer; not physical camera or deployed Alexa acceptance'}
    try:
        run(args.build_dir.resolve(), report)
    except Exception as error:
        report['error'] = type(error).__name__ + ': ' + str(error)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS' if report['passed'] else 'FAIL: ' + report.get('error', 'unknown'))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
