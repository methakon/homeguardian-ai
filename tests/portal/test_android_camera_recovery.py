"""Hermetic Android neighbor parser, API30 compile and JNI recovery wiring gates.

No device or network is accessed. Run directly with python3.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AndroidCameraRecovery(unittest.TestCase):
    def test_native_parser_and_credential_identity_refusals(self):
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as directory:
            binary = str(Path(directory) / 'neighbor-tests')
            subprocess.run(['c++', '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'src'), str(ROOT / 'tests/portal/test_android_neighbor_table.cpp'),
                            str(ROOT / 'src/camera/DeviceFingerprint.cpp'), '-o', binary], check=True, timeout=40)
            result = subprocess.run([binary], capture_output=True, text=True, check=True, timeout=5)
            self.assertIn('netlink/identity assertions (no network)', result.stdout)
            print(result.stdout.strip())

    def test_android_uses_netlink_and_existing_authorized_recovery_loop(self):
        locator = (ROOT / 'src/bridge/CameraLocator.h').read_text()
        jni = (ROOT / 'apk_service/jni/hg_edge_service_jni.cpp').read_text()
        self.assertIn('neighbor_netlink::read_table()', locator)
        self.assertIn('cfg.scan_enabled=true;', jni)
        self.assertIn('cfg.state_file = data_dir_ + "/camera_locator.state";', jni)
        self.assertIn('feed.maybe_rediscover(now)', jni)
        self.assertIn('std::mutex camera_authorization_mutex;', jni)
        self.assertIn('std::lock_guard<std::mutex> auth_guard(camera_authorization_mutex);', jni)
        self.assertIn('fresh.value("host",std::string())==cfg.host', jni)
        self.assertIn('fresh.value("mac",std::string())==cfg.mac', jni)
        self.assertIn('authorized_enrolled_mac_kernel_netlink', jni)
        self.assertNotIn('owner_approved_fixed_host_mac_no_rediscovery', jni)
        self.assertEqual(jni.count('camera_thread_=std::thread('), 1)

    def test_api30_compiles_the_actual_android_locator_backend(self):
        ndk = Path(os.environ.get('ANDROID_NDK_HOME', str(Path.home() / 'Android/Sdk/ndk/android-ndk-r26d')))
        compiler = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang++'
        if not compiler.exists():
            self.skipTest('API30 NDK unavailable; host tests do not prove Android compilation')
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as directory:
            source = Path(directory) / 'compile.cpp'
            source.write_text('#include "bridge/CameraLocator.h"\nstd::string check(){return homeguardian::portal::locator_detail::default_io().read_arp();}\n')
            subprocess.run([str(compiler), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                            '-c', str(source), '-o', str(Path(directory) / 'compile.o')], check=True, timeout=40)


if __name__ == '__main__':
    unittest.main()
