#!/usr/bin/env python3
"""Dry-run by default. Explicit --apply needs prior owner authorization.

Uses the operator's existing OCI SDK configuration; never accepts credentials or
client public IPs. Adds only the fixed roaming WireGuard rule to an exclusively
attached NSG. Existing rules are never deleted or replaced.
"""
import argparse
import json
import os
from pathlib import Path
from roaming_ingress_policy import plan_rule_additions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vnic-id", required=True)
    parser.add_argument("--nsg-id", required=True)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--snapshot", type=Path)
    args = parser.parse_args()
    if args.apply and not args.snapshot:
        parser.error("--apply requires a private --snapshot path")
    import oci
    client = oci.core.VirtualNetworkClient(oci.config.from_file(), timeout=(5, 12),
                                         retry_strategy=oci.retry.NoneRetryStrategy())
    vnic = client.get_vnic(args.vnic_id).data
    if args.nsg_id not in vnic.nsg_ids:
        raise RuntimeError("NSG is not attached to the expected VNIC")
    members = oci.pagination.list_call_get_all_results(
        client.list_network_security_group_vnics, args.nsg_id).data
    if len(members) != 1 or members[0].vnic_id != args.vnic_id:
        raise RuntimeError("NSG is shared or target ownership is uncertain")

    def inventory():
        return [oci.util.to_dict(rule) for rule in oci.pagination.list_call_get_all_results(
            client.list_network_security_group_security_rules, args.nsg_id).data]

    before = inventory()
    additions = plan_rule_additions(before)
    if not args.apply or not additions:
        print(json.dumps({"applied": False, "planned_additions": len(additions),
                          "existing_rules": len(before), "already_compliant": not additions}))
        return
    fd = os.open(args.snapshot, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "w") as snapshot:
        snapshot.write(json.dumps({"vnic_id": args.vnic_id, "nsg_id": args.nsg_id,
                                   "before": before}) + "\n")
        snapshot.flush()
        os.fsync(snapshot.fileno())
        rule = additions[0]
        port = rule["udp_options"]["destination_port_range"]
        model = oci.core.models.AddSecurityRuleDetails(
            direction=rule["direction"], protocol=rule["protocol"], source=rule["source"],
            source_type=rule["source_type"], is_stateless=rule["is_stateless"],
            description=rule["description"], udp_options=oci.core.models.UdpOptions(
                destination_port_range=oci.core.models.PortRange(min=port["min"], max=port["max"])))
        client.add_network_security_group_security_rules(args.nsg_id,
            oci.core.models.AddNetworkSecurityGroupSecurityRulesDetails(security_rules=[model]))
        after = inventory()
        snapshot.write(json.dumps({"after": after}) + "\n")
        snapshot.flush()
        os.fsync(snapshot.fileno())
        old = {r["id"]: r for r in before}
        new = {r["id"]: r for r in after}
        if any(new.get(k) != v for k, v in old.items()):
            raise RuntimeError("concurrent pre-existing rule change; inspect snapshot")
        added = [r for k, r in new.items() if k not in old]
        if len(added) != 1 or plan_rule_additions(added):
            raise RuntimeError("unexpected readback; inspect snapshot")
        print(json.dumps({"applied": True, "added_rule_ids": [r["id"] for r in added],
                          "preserved_rules": len(before), "roaming_policy_verified": True}))


if __name__ == "__main__":
    main()
