"""Off-device UI wiring/security contracts; not Android instrumentation tests."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "apk_service/src/org/homeguardian/edgeservice/MainActivity.java"


def method(source, name):
    start = re.search(r"(?:private|protected) void " + name + r"\(\)", source).start()
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[opening:end]


class BridgeUpdateContracts(unittest.TestCase):
    def test_ui_uses_platform_route_policy_before_start(self):
        body = method(SOURCE.read_text(), "onBridgeStart")
        self.assertIn("RouteSecurityPolicy.wireAndroidPlatform(this);", body)
        self.assertIn("String refusal = RouteSecurityPolicy.refusalReason(url);", body)
        self.assertNotIn("TransportPolicy.refusalReason", body)
        self.assertLess(body.index("if (refusal != null)"), body.index("startService(i)"))
        self.assertIn("return;", body)


    def test_explicit_enrollment_is_private_temporary_and_bridge_only(self):
        source = SOURCE.read_text()
        self.assertIn('button("Bridge enrollment",', source)
        self.assertIn("public void onClick(View v) { onBridgeEnrollment(); }", source)
        self.assertIn("private void onBridgeEnrollment()", source,
                      "missing explicit enrollment dialog")
        body = method(source, "onBridgeEnrollment")
        self.assertEqual(re.findall(r'payload\.put\("([^"]+)"', body),
                         ["device_id", "auth_token"])
        self.assertIn('payload.put("device_id", OutboundMcpBridge.deviceId(this));', body)
        self.assertIn('payload.put("auth_token", OutboundMcpBridge.authToken(this));', body)
        self.assertIn(".setMessage(payload.toString())", body)
        self.assertIn(".setPositiveButton(\"Dismiss\", null)", body)
        self.assertIn(".setCancelable(true)", body)
        self.assertLess(body.index("FLAG_SECURE"), body.index("enrollmentDialog.show()"))
        self.assertIn("ui.postDelayed(enrollmentTimeout, 30000L);", body)
        self.assertIn("setOnDismissListener", body)
        self.assertIn('((AlertDialog) dialog).setMessage("");', body)
        self.assertIn("ui.removeCallbacks(enrollmentTimeout);", body)
        self.assertIn("enrollmentDialog = null;", body)
        self.assertIn("dismissEnrollment();", method(source, "onPause"))
        for forbidden in ("Provisioner", "readToken", "Log.", "Clipboard", "setTextIsSelectable",
                          "EdgeState.lastError = t", "printStackTrace"):
            self.assertNotIn(forbidden, body)
        self.assertIn("enrollmentDialog.dismiss();", method(source, "dismissEnrollment"))


    def test_old_dialog_dismiss_cannot_clear_a_new_dialog(self):
        body = method(SOURCE.read_text(), "onBridgeEnrollment")
        self.assertIn('((AlertDialog) dialog).setMessage("");', body)
        self.assertIn("if (enrollmentDialog == dialog)", body)
        dismiss = method(SOURCE.read_text(), "dismissEnrollment")
        self.assertIn("enrollmentDialog = null;", dismiss)
        self.assertIn("ui.removeCallbacks(enrollmentTimeout);", dismiss)


if __name__ == "__main__":
    unittest.main()
