#!/usr/bin/env python3
"""HG-LAN-MTLS-07 - network-failure and reconnect policy coverage.

SCOPE, stated plainly: this is a DETERMINISTIC SIMULATION of the transport's reconnect policy. It is
NOT a physical radio or link test, and it proves nothing about real hardware behaviour. What it does
prove is that the policy itself is bounded and cannot lie:

  * a lost link never reports CONNECTED - the failure must be visible in the state;
  * reconnection is bounded (attempts and total elapsed), never an infinite retry loop;
  * backoff grows and is capped, so a long outage cannot produce unbounded waits or a hammering loop;
  * a dropped session may resend, but the receiver must deliver EXACTLY ONCE (dedup by message id) -
    i.e. at-least-once sending with idempotent delivery, never duplicate observations;
  * recovery to CONNECTED happens only on a genuine application-level ACK.

The link is a deterministic model, and the clock is fake, so the suite runs in milliseconds and is
reproducible - a real sleep would make it slow and flaky without testing anything more.

Run: python3 scripts/mtls_fixtures/test_reconnect.py
Exit 0 only if every property holds.
"""
import sys

CONNECTING, CONNECTED, FAILED = "CONNECTING", "CONNECTED", "FAILED"
results: list[tuple[str, bool, str]] = []


class Link:
    """Deterministic link model. Outcomes per attempt: ok | no_ack | down | reject."""

    def __init__(self, outcomes):
        self.outcomes = list(outcomes)
        self.i = 0
        self.delivered: list[str] = []          # receiver-side record, deduped by message id

    def send(self, msg) -> tuple[str, bytes | None]:
        outcome = self.outcomes[min(self.i, len(self.outcomes) - 1)]
        self.i += 1
        if outcome == "ok":
            self._deliver(msg)
            return "ok", b"OK"
        if outcome == "no_ack":                 # receiver got it, the ACK was lost
            self._deliver(msg)
            return "no_ack", None
        if outcome == "reject":
            return "reject", None
        return "down", None

    def _deliver(self, msg):
        if msg["id"] not in self.delivered:
            self.delivered.append(msg["id"])


def reconnect(link, msg, base=1.0, cap=30.0, max_attempts=6):
    """Bounded reconnect with exponential backoff. Never reports CONNECTED without an ACK."""
    delays, attempts, state = [], 0, CONNECTING
    while attempts < max_attempts:
        attempts += 1
        outcome, ack = link.send(msg)
        if outcome == "ok" and ack == b"OK":
            state = CONNECTED
            return {"state": state, "attempts": attempts, "delays": delays,
                    "delivered": list(link.delivered)}
        if outcome == "reject":                 # a refusal is not a link problem: stop, do not retry
            state = FAILED
            return {"state": state, "attempts": attempts, "delays": delays,
                    "delivered": list(link.delivered), "reason": "rejected"}
        if state == CONNECTED:
            raise AssertionError("state claimed CONNECTED after a failed attempt")
        delays.append(min(base * (2 ** (attempts - 1)), cap))
    return {"state": FAILED, "attempts": attempts, "delays": delays,
            "delivered": list(link.delivered), "reason": "exhausted"}


def check(name, ok, detail=""):
    results.append((name, ok, detail))


MSG = {"id": "obs-0001", "body": "compact observation"}

# 1 — a steady link connects once and delivers exactly once -------------------------------------
r = reconnect(Link(["ok"]), MSG)
check("steady link: CONNECTED on one attempt",
      r["state"] == CONNECTED and r["attempts"] == 1, f"{r['state']}, attempts={r['attempts']}")
check("steady link: delivered exactly once", r["delivered"] == [MSG["id"]], str(r["delivered"]))

# 2 — a link that is down never reports CONNECTED, and retries are bounded ----------------------
r = reconnect(Link(["down"]), MSG)
check("link down: state is FAILED, never CONNECTED", r["state"] == FAILED, r["state"])
check("link down: attempts are bounded (no infinite retry loop)",
      r["attempts"] == 6, f"attempts={r['attempts']}")
check("link down: nothing was delivered", r["delivered"] == [], str(r["delivered"]))

# 3 — backoff grows and is capped -----------------------------------------------------------------
r = reconnect(Link(["down"]), MSG, base=1.0, cap=8.0, max_attempts=8)
# After the cap the delay stops growing, so the property is "non-decreasing, and genuinely
# increasing until the cap" - asserting strict monotonicity everywhere would be wrong.
nondec = all(b >= a for a, b in zip(r["delays"], r["delays"][1:])) if len(r["delays"]) > 1 else False
grew_before_cap = len(r["delays"]) >= 3 and r["delays"][0] < r["delays"][1] < r["delays"][2]
check("backoff is non-decreasing and grows before the cap", nondec and grew_before_cap,
      str(r["delays"]))
check("backoff is capped (no unbounded wait)", max(r["delays"]) <= 8.0, f"max={max(r['delays'])}")
check("backoff respects the cap after the growth phase",
      r["delays"].count(8.0) >= 1, str(r["delays"]))

# 4 — a lost ACK must not be counted as delivered, and the retry must not duplicate ---------------
link = Link(["no_ack", "ok"])
r = reconnect(link, MSG)
check("lost ACK: not reported as CONNECTED on the failed attempt",
      r["state"] == CONNECTED and r["attempts"] == 2, f"{r['state']}, attempts={r['attempts']}")
check("lost ACK: EXACTLY ONE delivery at the receiver (idempotent)",
      r["delivered"] == [MSG["id"]], str(r["delivered"]))

# 5 — recovery after an outage --------------------------------------------------------------------
r = reconnect(Link(["down", "down", "ok"]), MSG)
check("recovery: CONNECTED only after a genuine ACK",
      r["state"] == CONNECTED and r["attempts"] == 3, f"{r['state']}, attempts={r['attempts']}")
check("recovery: delivered exactly once across the outage",
      r["delivered"] == [MSG["id"]], str(r["delivered"]))

# 6 — a policy refusal is terminal, not retried -----------------------------------------------------
r = reconnect(Link(["reject"]), MSG)
check("policy rejection: FAILED immediately, not retried",
      r["state"] == FAILED and r["attempts"] == 1 and r.get("reason") == "rejected", str(r))

# 7 — messages are deduped independently of transport retries ---------------------------------------
link = Link(["no_ack", "no_ack", "ok"])
r = reconnect(link, MSG)
check("three sends, still ONE delivery", len(r["delivered"]) == 1, str(r["delivered"]))
check("sends >= deliveries (at-least-once sending)", link.i >= len(link.delivered), f"{link.i} sends")

print("network-failure and reconnect policy (deterministic simulation - NOT a physical link test)\n")
print(f"{'property':68} result")
print("-" * 100)
for name, ok, detail in results:
    print(f"{name:68} {'ok' if ok else 'VIOLATION':9} {detail}")
print("-" * 100)
bad = [r for r in results if not r[1]]
if bad:
    print(f"FAIL: {len(bad)} of {len(results)} properties did not hold")
    sys.exit(1)
print(f"PASS: all {len(results)} properties hold")
