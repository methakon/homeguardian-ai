"""Executable host regression tests for the Android private-bridge route policy.

These run the PRODUCTION Java class (apk_service/.../RouteSecurityPolicy.java), not a copy of its
logic. The class is compiled against the real android.jar; a minimal android.util.Log stub is
placed first on the runtime classpath because android.jar's method bodies are "Stub!" throwers on
a host JVM.

No device, adb, network change or deployment is involved.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PRODUCTION = REPO / "apk_service/src/org/homeguardian/edgeservice/RouteSecurityPolicy.java"
TEST_DIR = Path(__file__).resolve().parent / "android_route"
TEST_JAVA = TEST_DIR / "RouteSecurityPolicyRegressionTest.java"
STUBS = TEST_DIR / "stubs"


def _java_home() -> Path:
    return Path(os.environ.get("JAVA_HOME", str(Path.home() / "android-jdk")))


def _android_jar() -> Path:
    sdk = Path(os.environ.get("ANDROID_HOME", str(Path.home() / "Android/Sdk")))
    for level in ("android-34", "android-30", "android-27"):
        candidate = sdk / "platforms" / level / "android.jar"
        if candidate.is_file():
            return candidate
    return sdk / "platforms" / "android-34" / "android.jar"


class RouteSecurityPolicyHostTest(unittest.TestCase):
    def test_production_policy_regression_suite(self) -> None:
        java_home = _java_home()
        javac = java_home / "bin" / "javac"
        java = java_home / "bin" / "java"
        android_jar = _android_jar()

        if not javac.is_file() or not java.is_file():
            self.fail(f"no JDK at {java_home} - set JAVA_HOME; this gate must not silently skip")
        if not android_jar.is_file():
            self.fail(f"no android.jar at {android_jar} - set ANDROID_HOME")

        with tempfile.TemporaryDirectory(prefix="hg-route-policy-") as tmp:
            out = Path(tmp)
            stubs_out = out / "stubs"
            prod_out = out / "prod"
            test_out = out / "test"

            def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    cmd, cwd=REPO, text=True, capture_output=True, timeout=180
                )

            stub_sources = sorted(str(p) for p in STUBS.rglob("*.java"))
            self.assertTrue(stub_sources, f"no stubs under {STUBS}")
            compile_stubs = run(
                [str(javac), "--release", "8", "-Xlint:-options", "-d", str(stubs_out)]
                + stub_sources
            )
            self.assertEqual(
                compile_stubs.returncode, 0,
                "stub compilation failed:\n" + compile_stubs.stdout + compile_stubs.stderr,
            )

            compile_prod = run(
                [
                    str(javac), "--release", "8", "-Xlint:-options",
                    "-classpath", str(android_jar), "-d", str(prod_out), str(PRODUCTION),
                ]
            )
            self.assertEqual(
                compile_prod.returncode, 0,
                "production class does not compile against android.jar:\n"
                + compile_prod.stdout + compile_prod.stderr,
            )

            compile_test = run(
                [
                    str(javac), "--release", "8", "-Xlint:-options",
                    "-classpath", f"{prod_out}:{stubs_out}", "-d", str(test_out),
                    str(TEST_JAVA),
                ]
            )
            self.assertEqual(
                compile_test.returncode, 0,
                "regression test does not compile against the production class:\n"
                + compile_test.stdout + compile_test.stderr,
            )

            executed = run(
                [
                    str(java), "-cp", f"{stubs_out}:{prod_out}:{test_out}",
                    "RouteSecurityPolicyRegressionTest",
                ]
            )
            self.assertEqual(
                executed.returncode, 0,
                "route-security regression failed:\n" + executed.stdout + executed.stderr,
            )
            self.assertIn("0 failed", executed.stdout)
            self.assertNotIn("FAIL  [", executed.stdout)
            # Guard against the harness silently running fewer cases than it announces.
            self.assertGreaterEqual(executed.stdout.count("PASS"), 57)


if __name__ == "__main__":
    unittest.main(verbosity=2)
