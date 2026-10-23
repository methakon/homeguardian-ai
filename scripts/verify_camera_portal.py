#!/usr/bin/env python3
"""Loopback camera-peer regression tests; never accesses a physical camera.

Requires a freshly built hg_alexa_bridge (HG_ALEXA_BRIDGE or hg_build/).
The peer serves synthetic JPEG marker bytes, not a captured camera image.
"""
import base64
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('HG_ALEXA_BRIDGE', ROOT / 'hg_build/hg_alexa_bridge')).resolve()


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class CameraPortalTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='hg-camera-peer-', dir=os.environ.get('TMPDIR'))
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.secret = secrets.token_hex(16)
        self.password = secrets.token_hex(16)
        self.expected_auth = 'Basic ' + base64.b64encode(('admin:' + self.password).encode()).decode()
        for name, value in [('secret', self.secret), ('password', self.password)]:
            (self.root / name).write_text(value + '\n')
            (self.root / name).chmod(0o600)
        (self.root / 'config.json').write_text(json.dumps({'media_capture_enabled': True}))
        (self.root / 'creds.json').write_text('{}')
        (self.root / 'creds.json').chmod(0o600)
        fixture_auth = self.expected_auth
        self.calls = []
        calls = self.calls

        class Peer(BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass

            def do_GET(self):
                calls.append(self.path)
                if self.headers.get('Authorization') != fixture_auth:
                    self.send_response(401)
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header('Content-Type', 'image/jpeg')
                self.end_headers()
                self.wfile.write(b'\xff\xd8synthetic-marker-fixture\xff\xd9')

        self.peer = ThreadingHTTPServer(('127.0.0.1', 0), Peer)
        thread = threading.Thread(target=self.peer.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.peer.server_close)
        self.addCleanup(self.peer.shutdown)

    @contextmanager
    def portal(self, *, password_file=True, plaintext=True, camera_port=None):
        listen = port()
        cmd = [str(BINARY), '--http', str(self.root / 'config.json'), str(self.root / 'creds.json'),
               '--port', str(listen), '--secret-file', str(self.root / 'secret'),
               '--uploads-dir', str(self.root / 'uploads'), '--camera-host', '127.0.0.1',
               '--camera-port', str(camera_port if camera_port is not None else self.peer.server_port)]
        if plaintext:
            cmd.append('--camera-allow-plaintext')
        if password_file:
            cmd += ['--camera-password-file', str(self.root / 'password')]
        env = {k: v for k, v in os.environ.items() if not k.startswith('HG_')}
        with subprocess.Popen(cmd, cwd=self.root, env=env, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL) as proc:
            try:
                for _ in range(60):
                    if proc.poll() is not None:
                        break
                    with socket.socket() as sock:
                        if sock.connect_ex(('127.0.0.1', listen)) == 0:
                            break
                    time.sleep(.05)
                yield proc, 'http://127.0.0.1:' + str(listen)
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=5)

    def request(self, base, path, authorized=True):
        headers = {'Origin': base}
        if authorized:
            headers['X-HG-Secret'] = self.secret
        req = urllib.request.Request(base + path, headers=headers)
        try:
            with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(req, timeout=5) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as error:
            try:
                return error.code, error.read()
            finally:
                error.close()

    def test_password_file_used_and_query_frame_supported(self):
        with self.portal() as (proc, base):
            self.assertIsNone(proc.poll())
            status, body = self.request(base, '/api/camera/frame?t=1')
            self.assertEqual(status, 200, 'camera password file was not used for the peer request')
            self.assertTrue(body.startswith(b'\xff\xd8'))
            status, body = self.request(base, '/api/camera/status')
            self.assertEqual(status, 200)
            result = json.loads(body)
            self.assertFalse(result['camera']['is_continuous_video'])
            self.assertFalse(result['camera']['analysis_connected'])
            self.assertEqual(result['camera']['state'], 'LIVE')
            self.assertNotIn(self.password, body.decode())

    def test_camera_request_without_operator_authorization_never_fetches(self):
        with self.portal() as (_, base):
            self.assertEqual(self.request(base, '/api/camera/frame?t=2', False)[0], 401)
            self.assertEqual(self.calls, [])

    def test_plaintext_requires_explicit_opt_in(self):
        with self.portal(plaintext=False) as (proc, _):
            self.assertEqual(proc.poll(), 3, 'configured plaintext camera must fail closed without opt-in')
            self.assertEqual(self.calls, [])

    def test_capture_kill_switch_prevents_camera_fetch(self):
        (self.root / 'config.json').write_text(json.dumps({'media_capture_enabled': False}))
        with self.portal() as (_, base):
            self.assertEqual(self.request(base, '/api/camera/frame?t=3')[0], 503)
            self.assertEqual(self.calls, [])

    def test_missing_password_file_fails_startup(self):
        (self.root / 'password').unlink()
        with self.portal() as (proc, _):
            self.assertEqual(proc.poll(), 3, 'missing credential file must not silently become an empty password')

    def test_invalid_camera_port_fails_cleanly(self):
        with self.portal(camera_port='invalid') as (proc, _):
            self.assertEqual(proc.poll(), 3, 'malformed camera configuration must not abort the process')


if __name__ == '__main__':
    unittest.main(verbosity=2)
