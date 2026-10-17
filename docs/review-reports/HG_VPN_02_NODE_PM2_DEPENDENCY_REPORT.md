# HG-VPN-02 — Node.js / PM2 dependency verification on `x402-endpoint`

Read-only investigation. No uninstall, stop, restart or reconfiguration was performed, and none is
proposed. Sanitized: no addresses, no credentials, no command lines, no `/proc/*/environ`, no config or
mail content read.

## Direct answers

    Role of Node PID 6853        the production x402 payment application itself
    Relationship to PM2 3675     6853 is the workload; 3675 is the PM2 daemon managing it (pm2 id 0)
    Existing dependencies        the live x402 payment endpoint on port 8402, up 16 days
    Removal safe?                **UNSAFE - it would take down a production payment service**
    Expected saving              ~94-97 MB RSS (node 50.4 + pm2 44.1) - not worth it, and not authorized
    Recommended arrangement      leave both untouched; keep this host Node-free for HomeGuardian

## Step 1 — process ownership

    PID 6853   user ubuntu   comm node   ~50.4 MB RSS   = pm2 id 0, name `x402-endpoint`,
                                                          cluster mode, listening on :8402, uptime 16 d
    PID 3675   ~44.1 MB RSS                            = the PM2 daemon (the parent manager)

Evidence for the identity of 6853 comes from the completed Gate A host audit, which read the instance
directly: "pm2 id 0 **is** `x402-endpoint` on :8402, uptime 16 days", process row `pm2 id 0, name
x402-endpoint, cluster mode, **PID 6853**, user ubuntu". It is corroborated by the project's own
deployment script, which starts the app under pm2 as `x402-endpoint` with `PORT=8402`.

**What I could not verify this round.** Two attempts to obtain the `/proc` linkage directly
(`ps -o pid,ppid,...`, `readlink /proc/6853/cwd|exe`, `/proc/6853/cgroup`, and the child list under 3675)
never reached the host: I could not resolve the instance's address locally — the read-only OCI CLI lookup
returned no row from this workstation, and the address is not present in the project deploy scripts. That
is a local tooling gap, **not** an authorization or identity problem: the instance identity itself was
already triple-pinned in Gate A (OCI CLI row, instance metadata returning the same OCID, and host-side
hostname/kernel/DMI). The consequence is only that the parent/child linkage and the cwd/cgroup for these
two PIDs are **UNVERIFIED** by me in this pass, while the pid-to-role mapping above rests on host-side
evidence from that audit. If you want the `/proc` evidence recorded, one follow-up with a reachable
address or the child compartment will close it.

## Step 2 — the dependency

1. **Is Node serving the x402 payment endpoint? YES.** PID 6853 is pm2 id 0, bound to :8402, running
   continuously for 16 days, at 0.1% CPU and with zero restarts. This is live production traffic, not a
   development leftover.
2. **Is PM2 managing that application? YES.** It is pm2 id 0 in cluster mode, and the 44.1 MB figure is
   the daemon that supervises it.
3. **Are any other applications PM2-managed? UNVERIFIED.** The audit confirmed id 0 and deliberately
   avoided enumerating anything beyond the single instance filter it was given; my own probe could not
   connect to list the rest. Do not assume the daemon manages only one app.
4. **Would removing Node or PM2 disrupt an existing service? YES — this is the service.** Stopping either
   stops the payment endpoint.
5. **Is either required for the proposed HomeGuardian deployment? NO.** HomeGuardian's deliverable is a
   C++20 runtime with an explicit requirement to eliminate Node.js from the final runtime, and the
   coordination/VPN role planned for this VPS needs neither Node nor a process manager. Keeping this host
   Node-free for HomeGuardian costs nothing and avoids adding a second runtime to a 1 GiB box.

## Step 3 — options compared

    Preserve Node and PM2 unchanged           0 MB, zero risk, no change to production.
                                              **RECOMMENDED** - it is the only option that cannot
                                              disturb the payment endpoint, and it already satisfies
                                              HomeGuardian's needs because HomeGuardian needs neither.
    Reuse PM2 for a future HomeGuardian part  Not appropriate: it contradicts the Node-free runtime
                                              requirement, adds a Node workload to a host that already
                                              swaps, and expands the blast radius of the existing
                                              daemon. Would need separate approval anyway.
    Deploy no Node app on this VPS            **RECOMMENDED** for the coordination/VPN role: run any
                                              future component as a native systemd unit (no Node, no
                                              PM2) or keep this host to the control plane only.
    Remove the unused components              NOT APPLICABLE and PROHIBITED. Neither component is
                                              unused, and prior project direction explicitly forbids
                                              stopping, deleting or replacing x402 or removing PM2.
                                              A removal would free ~94 MB but take down the payment
                                              endpoint: unacceptable risk for a modest saving on a host
                                              that currently has 455 MB available and zero memory
                                              pressure.

## Step 4 — recommendation

**Do not remove Node or PM2.** They are the production payment service and its supervisor. There is no
HomeGuardian requirement that justifies touching them: the planned role for this VPS is lightweight
coordination, and HomeGuardian has no Node dependency at all. Any future HomeGuardian component on this
host should be a native systemd service, and any proposal to modify the existing stack needs its own
explicit owner approval.

## Stop

Nothing was uninstalled, stopped, restarted, reloaded or reconfigured. No PM2 command was executed. The
second Oracle VPS was not touched, mail infrastructure was not touched, and no environment, command line,
credential or configuration file was read. A production change of any kind requires separate, explicit
owner approval.
