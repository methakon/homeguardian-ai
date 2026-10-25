import copy
import importlib.util
import ipaddress
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("roaming_ingress_policy", ROOT / "deploy/coordinator/roaming_ingress_policy.py")
assert SPEC is not None and SPEC.loader is not None
POLICY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POLICY)


class RoamingIngressPolicyTest(unittest.TestCase):
    def test_dynamic_sources_not_fixed_home_addresses(self):
        network = ipaddress.ip_network(POLICY.roaming_wireguard_rule()["source"])
        for address in ["198.51.100.9", "203.0.113.201", "192.0.2.17"]:
            self.assertIn(ipaddress.ip_address(address), network)

    def test_only_exact_udp_vpn_port(self):
        rule = POLICY.roaming_wireguard_rule()
        self.assertEqual(rule["direction"], "INGRESS")
        self.assertEqual(rule["protocol"], "17")
        self.assertEqual(rule["udp_options"], {"destination_port_range": {"min": 51820, "max": 51820}})
        self.assertEqual(rule["source_type"], "CIDR_BLOCK")
        self.assertIs(rule["is_stateless"], False)
        self.assertNotIn("tcp_options", rule)

    def test_old_home_rules_are_preserved_not_rewritten(self):
        old = [dict(POLICY.roaming_wireguard_rule(), source="198.51.100.9/32"),
               {"direction": "INGRESS", "protocol": "6", "source": "192.0.2.0/24"}]
        before = copy.deepcopy(old)
        self.assertEqual(POLICY.plan_rule_additions(old), [POLICY.roaming_wireguard_rule()])
        self.assertEqual(old, before)

    def test_already_correct_is_idempotent(self):
        self.assertEqual(POLICY.plan_rule_additions([POLICY.roaming_wireguard_rule()]), [])

    def test_unrelated_or_incomplete_rules_do_not_hide_missing_policy(self):
        for update in [{"direction": "EGRESS"}, {"protocol": "6"}, {"source": "198.51.100.9/32"},
                       {"is_stateless": True}, {"source_type": "NETWORK_SECURITY_GROUP"},
                       {"udp_options": {"destination_port_range": {"min": 8790, "max": 8790}}},
                       {"udp_options": {"destination_port_range": {"min": 51820, "max": 51820},
                                        "source_port_range": {"min": 1234, "max": 1234}}}]:
            with self.subTest(update=update):
                rule = dict(POLICY.roaming_wireguard_rule(), **update)
                self.assertEqual(POLICY.plan_rule_additions([rule]), [POLICY.roaming_wireguard_rule()])

    def test_inventory_must_be_valid(self):
        for invalid in [None, {}, "rules", [None], ["rule"]]:
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                POLICY.plan_rule_additions(invalid)

    def test_callers_cannot_mutate_next_plan(self):
        first = POLICY.plan_rule_additions([])
        first[0]["udp_options"]["destination_port_range"]["max"] = 65535
        self.assertEqual(POLICY.plan_rule_additions([])[0]["udp_options"]["destination_port_range"]["max"], 51820)


if __name__ == "__main__":
    unittest.main(verbosity=2)
