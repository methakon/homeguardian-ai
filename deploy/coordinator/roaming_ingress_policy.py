"""Roaming WireGuard cloud-ingress policy: no client public-IP dependency."""
import copy


def roaming_wireguard_rule():
    """Only the authenticated VPN entry point, never an application TCP port."""
    return {
        "direction": "INGRESS",
        "protocol": "17",
        "source": "0.0.0.0/0",
        "source_type": "CIDR_BLOCK",
        "is_stateless": False,
        "udp_options": {"destination_port_range": {"min": 51820, "max": 51820}},
        "description": "HomeGuardian roaming WireGuard peers; cryptographic authentication required",
    }


def plan_rule_additions(existing_rules):
    """Return an additive, idempotent plan; never rewrite unrelated live rules."""
    if not isinstance(existing_rules, list) or any(not isinstance(r, dict) for r in existing_rules):
        raise ValueError("invalid security-rule inventory")
    wanted = roaming_wireguard_rule()
    for rule in existing_rules:
        udp = rule.get("udp_options") or {}
        if (rule.get("direction") == "INGRESS" and rule.get("protocol") == "17"
                and rule.get("source") == "0.0.0.0/0"
                and rule.get("source_type") == "CIDR_BLOCK"
                and rule.get("is_stateless") is False
                and udp.get("destination_port_range") == {"min": 51820, "max": 51820}
                and udp.get("source_port_range") is None):
            return []
    return [copy.deepcopy(wanted)]
