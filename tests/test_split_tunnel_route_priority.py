"""TDD: split-tunnel routing must follow kernel longest-prefix match, not first-cover."""
import re
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
TEST = ROOT / "scripts/bridge_ws_compat/RouteSecurityPolicyTest.java"
POLICY = ROOT / "apk_service/src/org/homeguardian/edgeservice/RouteSecurityPolicy.java"


class SplitTunnelLongestPrefix(unittest.TestCase):
    def test_regression_case_is_executable_and_not_vacuous(self):
        """The observable real-device shape: Wi-Fi carries 0.0.0.0/0, VPN carries a /32."""
        source = TEST.read_text()
        self.assertIn("=== I. SPLIT TUNNEL WITH A NON-VPN DEFAULT ROUTE", source)
        self.assertIn('platform(net("wlan0", false, "192.168.1.0/24", "0.0.0.0/0"),', source)
        self.assertIn('net("vpn",   true,  "192.168.77.1/32"));', source)
        self.assertIn('allow("VPN /32 wins against a non-VPN default route"', source)
        self.assertIn('"ws://192.168.77.1:18994/");', source)
        self.assertIn('refuse("equal specificity on both paths stays refused"', source)
        self.assertIn('"egress unprotected");', source)
        # A suite that stopped asserting the refusing direction would pass by refusing everything.
        self.assertGreaterEqual(source.count("refuse("), 12)
        self.assertGreaterEqual(source.count("allow("), 6)

    def test_policy_compares_prefix_length_and_keeps_fail_closed_paths(self):
        body = POLICY.read_text()
        self.assertIn("bestPrefix", body, "policy must rank covering prefixes")
        self.assertIn("nonVpnBest >= vpnBest", body)
        # The fail-closed order that existing groups depend on must survive.
        for required in ('"refused: no VPN network is active, and "',
                         '"refused: cannot verify routing ("',
                         '"refused: no VPN network routes "',
                         '" is routed by non-VPN network \'"'):
            self.assertIn(required, body)


if __name__ == "__main__":
    unittest.main()
