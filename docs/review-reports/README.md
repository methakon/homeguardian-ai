# HomeGuardian — Infrastructure Feasibility and Retirement Review (Public Summary)

**Status:** Architecture assessed; no production retirement or VPN deployment performed.

A limited read-only capacity and dependency assessment was completed for the owner-designated cloud host. A full operational manifest containing instance identifiers, protected-service inventory, proposed production removal commands and recovery procedures was moved to **restricted owner-controlled storage** on 17 October 2026.

## Findings suitable for public discussion

- A lightweight kernel WireGuard control/management tunnel appears feasible on the existing small Linux VM, subject to separate owner approval and capacity/safety monitoring.
- A central VPN hub would forward encrypted media over the cloud host's own uplink; this does not satisfy HomeGuardian's default requirement to avoid proxying CCTV media through the gateway.
- Development-time Node/PM2 payment components were assessed as isolated from the retained mail software. Retirement is **not yet executed**; operations must be reviewed separately with protected backups, verified dependencies, mail-service checks and rollback.
- The owner's mail server, existing certificate renewal, DNS, shared cloud networking, firewall and other cloud instances are out of scope for destructive changes.
- Several external-service and secret-handling decisions remain unresolved; these block unattended cleanup.
- The HomeGuardian production architecture uses durable logical device identities, explicit authorization, outbound site connectors and direct encrypted media when feasible; NAT constraints can require an explicitly approved relay.

**Release caveat:** This summary is not evidence of an installed VPN, a completed x402 retirement, a consumer Alexa camera integration, or a passed security-release audit.

## Restricted evidence

The original operational source reports are archived in the project owner's **unshared Google Drive document** (access granted only by the owner, not linked here). Do not republish instance IDs, IPs, tunnel credential paths, mail configuration details or executable deletion runbooks in this public repository.

**Historical exposure:** Prior commits on this branch contained fuller reports. Removing them from the current branch tip does not remove copies in Git history; the maintainers should plan a coordinated history review and any necessary credential rotation rather than force-pushing without coordination.
