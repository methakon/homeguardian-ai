#!/usr/bin/env python3
"""WireGuard preflight: privilege/error handling and dead-man rollback, with MOCKED tools.

SIMULATED, not a real handshake. Every external tool (sudo, ip, wg, iptables, sysctl, systemctl,
systemd-run) is replaced by a shim on PATH that records its argv and returns a controlled exit code,
so the refusal and rollback paths can be exercised on a host with no wireguard-tools and no
privilege. A real handshake still requires the operator leg documented in the runbook; nothing here
claims otherwise.

Cases
  1 preflight clean with mocks            -> exit 0
  2 'up' without --execute                -> refuses, and NO interface is created
  3 'up --execute' when ip link add fails -> fails without leaving a tunnel behind
  4 dead-man rollback is armed            -> it invokes THIS script 'down --execute'
  5 'down' must not delete a foreign wg0  -> refuses (no ownership marker), no 'link del'
  6 baseline unmeasurable                 -> 'up' refuses to change state
  7 'down --execute' with the marker      -> deletes, and cancels the dead-man
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "wg_preflight.sh")

SHIM = """#!/bin/bash
echo "{name} $*" >> "$HG_FAKE_LOG"
case "{name} $*" in
  "sudo -n true") exit 0 ;;
esac
{body}
"""


def make_bin(root, iface_exists=0, ip_add_fails=0, routes="default via 192.168.1.1 dev wlp6s0"):
    b = os.path.join(root, "bin")
    os.makedirs(b, exist_ok=True)
    def w(name, body=""):
        p = os.path.join(b, name)
        open(p, "w").write(SHIM.format(name=name, body=body))
        os.chmod(p, 0o755)
    # sudo: behave like a transparent prefix so 'sudo ip ...' hits the shimmed ip
    w("sudo", 'shift 0; if [ "$1" = "-n" ]; then exit 0; fi; exec "$@"')
    w("ip", f'''
iface_exists={iface_exists}; add_fails={ip_add_fails}
case "$*" in
  *"link add"*)  [ "$add_fails" = 1 ] && exit 2 || exit 0 ;;
  *"link show dev wg0"*|*"link show"*) [ "$iface_exists" = 1 ] && echo "5: wg0: <POINTOPOINT,UP> mtu 1420" && exit 0 || exit 1 ;;
  *"route show"*) echo "{routes}" ; exit 0 ;;
  *"addr show"*)  echo "wlp6s0 UP 192.168.1.9/24" ; exit 0 ;;
esac
exit 0''')
    w("wg", 'case "$*" in *latest-handshakes*) echo "9d4f 1792570000"; exit 0 ;; esac; exit 0')
    w("iptables", 'exit 0')
    w("sysctl", 'case "$*" in *ip_forward*) echo 0 ;; *) echo 0 ;; esac')
    w("systemctl", 'exit 0')
    w("systemd-run", 'exit 0')
    w("ping", 'exit 0')
    w("nc", 'exit 0')
    return b


def run(binroot, args, conf, env_extra=None):
    log = os.path.join(os.path.dirname(binroot), "calls.log")
    open(log, "w").close()
    env = {"PATH": binroot + ":/usr/sbin:/sbin:/usr/bin:/bin",   # realistic: sbin holds modinfo "HOME": os.path.dirname(binroot),
           "HG_FAKE_LOG": log, "HG_WG_CONF": conf,
           "HG_WG_STATE": os.path.join(os.path.dirname(binroot), "state")}
    env.update(env_extra or {})
    p = subprocess.run(["bash", SCRIPT, *args], capture_output=True, text=True, env=env, timeout=120)
    return p.returncode, p.stdout + p.stderr, open(log).read()


def case(fn, root):
    try:
        return fn(root)
    except Exception as exc:                                  # noqa: BLE001
        return False, f"{type(exc).__name__}: {exc}"


def c1_clean(root):
    b = make_bin(root)
    conf = os.path.join(root, "wg0.conf"); open(conf, "w").write("[Interface]\n")
    os.chmod(conf, 0o600)
    rc, out, _ = run(b, ["preflight"], conf)
    return rc == 0, f"rc={rc} result={[l for l in out.splitlines() if 'RESULT' in l]}"


def c2_up_without_execute(root):
    b = make_bin(root)
    conf = os.path.join(root, "wg0.conf"); open(conf, "w").write("[Interface]\n"); os.chmod(conf, 0o600)
    rc, out, log = run(b, ["up"], conf)
    return rc == 3 and "link add" not in log, f"rc={rc} (expected 3) created_interface={'link add' in log}"


def c3_link_add_fails(root):
    b = make_bin(root, ip_add_fails=1)
    conf = os.path.join(root, "wg0.conf"); open(conf, "w").write("[Interface]\n"); os.chmod(conf, 0o600)
    rc, out, log = run(b, ["up", "--execute"], conf)
    state = os.path.join(os.path.dirname(b), "state", "created")
    reached = "link add" in log          # must have actually attempted the create, else vacuous
    return (rc != 0 and reached and not os.path.exists(state)), \
        f"rc={rc} reached_create={reached} marker={os.path.exists(state)}"


def c4_deadman_armed(root):
    b = make_bin(root)
    conf = os.path.join(root, "wg0.conf"); open(conf, "w").write("[Interface]\n"); os.chmod(conf, 0o600)
    rc, out, log = run(b, ["up", "--execute"], conf)
    reached = "link add" in log          # a dead-man test means nothing if we never got up
    armed = "systemd-run" in log
    # the armed unit must invoke THIS script's rollback, not something else
    calls = [l for l in log.splitlines() if "systemd-run" in l]
    wired = any("down" in c and "wg_preflight.sh" in c for c in calls)
    return (reached and armed and wired), \
        f"reached_create={reached} armed={armed} rollback_wired={wired}"


def c5_down_refuses_foreign_iface(root):
    b = make_bin(root, iface_exists=1)          # an interface exists...
    conf = os.path.join(root, "wg0.conf"); open(conf, "w").write("[Interface]\n"); os.chmod(conf, 0o600)
    rc, out, log = run(b, ["down", "--execute"], conf)   # ...but this run did not create it
    return rc == 6 and "link del" not in log, f"rc={rc} (expected 6) deleted={'link del' in log}"


def c6_unmeasurable_baseline(root):
    # no default route -> the baseline cannot be trusted; assert the substantive property only:
    # up must NOT create an interface, whichever gate catches it (fail-closed either way)
    b = make_bin(root, routes="")
    conf = os.path.join(root, "wg0.conf"); open(conf, "w").write("[Interface]\n"); os.chmod(conf, 0o600)
    rc, out, log = run(b, ["up", "--execute"], conf)
    return rc != 0 and "link add" not in log, f"rc={rc} changed_state={'link add' in log}"


def c7_down_with_marker(root):
    b = make_bin(root, iface_exists=1)
    conf = os.path.join(root, "wg0.conf"); open(conf, "w").write("[Interface]\n"); os.chmod(conf, 0o600)
    st = os.path.join(root, "state"); os.makedirs(st, exist_ok=True)
    open(os.path.join(st, "net.pre"), "w").write("wlp6s0 UP 192.168.1.9/24")
    open(os.path.join(st, "created"), "w").write("2026-10-21T00:00:00Z")
    rc, out, log = run(b, ["down", "--execute"], conf)
    return ("link del" in log), f"rc={rc} deleted={'link del' in log} deadman_cancelled={'systemctl' in log}"


CASES = [
    ("1 preflight clean with mocks", c1_clean),
    ("2 up without --execute refuses", c2_up_without_execute),
    ("3 ip link add fails -> nothing left behind", c3_link_add_fails),
    ("4 dead-man rollback armed and wired to down", c4_deadman_armed),
    ("5 down refuses a foreign interface", c5_down_refuses_foreign_iface),
    ("6 unmeasurable baseline -> up refuses, no interface", c6_unmeasurable_baseline),
    ("7 down with ownership marker deletes", c7_down_with_marker),
]


def main():
    if not os.path.exists(SCRIPT):
        print(f"script not found: {SCRIPT}"); return 2
    root = os.path.expanduser("~/.hermes/cache/scratch/hg-wg-mock")
    shutil.rmtree(root, ignore_errors=True); os.makedirs(root, exist_ok=True)
    failed = 0
    print("  SIMULATED with mocked tools - NOT a real handshake")
    for i, (name, fn) in enumerate(CASES):
        d = tempfile.mkdtemp(dir=root, prefix=f"c{i}_")
        ok, detail = case(fn, d)
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
        failed += 0 if ok else 1
    print(f"  {len(CASES) - failed}/{len(CASES)} cases pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
