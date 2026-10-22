# HG-RECOVERY-SECURITY-43 — stage 2: the scope predicate, measured; and a correction to stage 1

Nonexecuting. The predicate was evaluated **as written** — no guard imported, no task admitted, nothing
executed. Read-only, no cherry-picks, no worker-tree crossings.

## Finding: path traversal bypasses scope confinement — in BOTH revisions

`task_guard.py` at the integration SHA (`8f40870a`, blob `9e956af9`) line 525, and in **my own original**
(blob `4985e5b3`) line **153**, the predicate is identical:

```python
allowed = tuple(cfg["authorized_scope"])
for scoped in task["allowed_scope"]:
    if not any(scoped == a or scoped.startswith(a) for a in allowed):
        raise Refused("SCOPE_WIDENING", ...)
```

A raw `str.startswith` on **un-normalized** strings. Evaluated as written with
`authorized_scope = ["scripts/worker/"]`:

```
ADMITTED  legitimate in-scope file     scripts/worker/test_poller.py
ADMITTED  PATH TRAVERSAL               scripts/worker/../../etc/shadow
ADMITTED  ENCODED TRAVERSAL            scripts/worker/..%2f..%2fetc/passwd
ADMITTED  TRAVERSAL to sibling tree    scripts/worker/../secrets/creds.json
refused   plain out-of-scope           etc/shadow
refused   shorter sibling              scripts/work
refused   SIBLING PREFIX               scripts/worker_evil/payload.sh
```

Corroboration: `normpath`, `realpath`, `commonpath`, `is_relative_to` and `resolve` each occur **zero**
times on that path in the altered copy.

## Correction to my stage-1 verdict — the scope check was NOT introduced by the integration branch

Stage 1 said the fallback **and the prefix scope check** were introduced by the integration-branch
modification. **The fallback part stands; the scope-check part was wrong.** The prefix predicate is
present in my original at line 153, in the same form. So this is a **shared defect in code I wrote**, not
a regression added by someone else, and I am correcting it rather than letting a wrong attribution stand
in the recovery matrix.

What the integration branch actually added: the SQLite approval ledger, `TrustedDispatchContext`, and
**two `test_fixture` fallback sites** — those remain its delta. The scope predicate is common ancestry.

## Refining the sibling-prefix claim: conditional, not universal

The assignment lists sibling prefixes as an admission vector. Measured, with an authorized scope ending
in `/` it is **refused** — `scripts/worker_evil/…` does not start with `scripts/worker/`. It admits only
when the configured authorized scope **lacks a trailing slash** (e.g. `scripts/worker` against
`scripts/worker_evil/…`). So the vector is **config-dependent**: present for `scripts/worker`, absent for
`scripts/worker/`. I am reporting it as conditional rather than repeating it as universal.

## Status after stage 2

| item | status |
|---|---|
| traversal bypass (`../`, encoded) | **CONFIRMED** by evaluating the predicate as written; affects **both** blobs |
| sibling-prefix bypass | **CONDITIONAL** — present only without a trailing slash; refused with one |
| symlink escape | **NOT TESTED** — needs a filesystem, so it is outside a predicate-level eval |
| empty / non-dict `approvals`, malformed payload | **NOT YET RUN** (approval path, not the scope predicate) |
| protected op with missing/false `requires_approval` | **NOT YET RUN** |
| forged author / verified actor / no context | **fallback CONFIRMED statically** in stage 1; CLI admission **NOT YET RUN** |
| consume-once concurrency | **NOT YET RUN** |
| red/green admission tests | **NOT RUN** |

The predicate-level result is deliberately scoped: it proves the **check** admits traversal, not that a
full CLI path reaches an ADMIT. That end-to-end claim still requires the nonexecuting CLI repros above,
which are unrun.

**P0 remains OPEN.** The fix for the scope predicate is independent of the fallback fix: canonicalize
(`realpath`/`commonpath`) and compare against the **resolved** authorized root, rejecting anything that
escapes it — and apply the same rule to both the scope check and the protected-target derivation.
