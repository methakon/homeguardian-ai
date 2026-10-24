"""Host regression: compile and execute the actual Android outbound relay, no device access."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent / 'android_session'


class AndroidMcpSessionTest(unittest.TestCase):
    def test_production_relay_session_regressions(self):
        jdk = Path(os.environ.get('JAVA_HOME', str(Path.home() / 'android-jdk')))
        compat = REPO / 'scripts/bridge_ws_compat/stubs'
        sources = list((compat / 'android').rglob('*.java'))
        # Use the richer JSON stubs: strings that look like JSON must stay strings.
        sources += list((REPO / 'apk/tests/stubs/org/json').glob('*.java'))
        sources += list(HERE.rglob('*.java'))
        prod = REPO / 'apk_service/src/org/homeguardian/edgeservice'
        sources += [prod / 'OutboundMcpBridge.java', prod / 'RouteSecurityPolicy.java']
        scratch = Path(os.environ.get('TMPDIR', str(Path.home() / '.hermes/cache/scratch')))
        scratch.mkdir(parents=True, exist_ok=True)
        sdk = Path(os.environ.get('ANDROID_HOME', str(Path.home() / 'Android/Sdk')))
        android_jar = next((p for level in ('android-34', 'android-30', 'android-27')
                            if (p := sdk / 'platforms' / level / 'android.jar').is_file()), None)
        self.assertIsNotNone(android_jar, 'set ANDROID_HOME: real platform compilation must not skip')
        # Keep the sole host credential-source stub pinned to actual production target constants.
        provisioner = (prod / 'Provisioner.java').read_text()
        self.assertIn('public static final int DEFAULT_PORT = 8790;', provisioner)
        self.assertIn('public static final String LOOPBACK = "127.0.0.1";', provisioner)
        with tempfile.TemporaryDirectory(prefix='hg-mcp-session-', dir=scratch) as tmp:
            commands = [
                [str(jdk / 'bin/javac'), '--release', '8', '-cp', str(android_jar),
                 '-d', str(Path(tmp) / 'platform-check'), str(prod / 'OutboundMcpBridge.java'),
                 str(prod / 'RouteSecurityPolicy.java'), str(prod / 'Provisioner.java')],
                [str(jdk / 'bin/javac'), '-d', tmp] + [str(p) for p in sources],
                [str(jdk / 'bin/java'), '-cp', tmp, 'AndroidMcpSessionRegressionTest'],
            ]
            for command in commands:
                result = subprocess.run(command, cwd=REPO, capture_output=True, text=True, timeout=60)
                print(result.stdout, end='')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                if command == commands[-1]:
                    self.assertIn('13 passed, 0 failed', result.stdout)
                    self.assertEqual(sum(line.startswith('PASS ') for line in result.stdout.splitlines()), 13)


if __name__ == '__main__':
    unittest.main(verbosity=2)
