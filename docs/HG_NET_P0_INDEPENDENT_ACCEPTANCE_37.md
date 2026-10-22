# HG-NET-P0-INDEPENDENT-ACCEPTANCE-37 — independent adversarial acceptance, stage 1 (static)

Read-only. Their code was extracted at the SHA into my scratch via the API and read there; **no
Antigravity/Hermes2/Hermes3 worktree was touched**, no privileged Docker operation, no old scan repeated.

`SHA under test: 8f40870a3f30fa66740ec895dca6498fa4cbe05f` (independently confirmed as the live remote
tip of `dev/production-integration`).

## P0 BLOCKER — CONFIRMED by reading the code at the SHA

The reported fallback is real, and there are **two** of them, as claimed. Exact locations:

```
task_guard.py:486    def validate(..., dispatch_context: TrustedDispatchContext | None = None, ...)
task_guard.py:510    author = task.get("author")                       <- from the task, i.e. caller data
task_guard.py:514    if dispatch_context is not None and author:       <- mismatch check is CONDITIONAL
task_guard.py:516        raise Refused("ACTOR_MISMATCH", ...)
task_guard.py:563    effective_context = dispatch_context
task_guard.py:566    ... else: effective_context = TrustedDispatchContext(..., auth_method="test_fixture")
task_guard.py:583/586  the SAME fallback constructed a second time
task_guard.py:81     "test_fixture" is a member of VALID_AUTH_METHODS
```

Three consequences, each independently readable:

1. **The mismatch guard is conditional**, not unconditional: `ACTOR_MISMATCH` can only fire when a
   `TrustedDispatchContext` was supplied. With `dispatch_context=None` there is nothing to mismatch, so
   the weakest path is the one with no cross-check.
2. **The fallback fabricates a trusted context from untrusted input** — `auth_method="test_fixture"` is
   an accepted method, so downstream code cannot distinguish "a test fixture authenticated this" from
   "no authentication happened".
3. **It appears twice**, so a fix must cover both sites; patching one leaves the other.

## `task_envelope.py` — CONFIRMED, and worse than the report says

```
task_envelope.py:76    author = ((comment.get("author") or {}).get("login") or "").strip()
task_envelope.py:79    if author not in policy["allowed_authors"]: raise EnvelopeRefused(...)
task_envelope.py:117   # Construct authenticated dispatch context from verified comment author
task_envelope.py:119       actor_id=author
task_envelope.py:121       auth_method="github_issue_comment_verified_author"
task_envelope.py:128   task['author'] = author
```

The comment on line 117 says the context is built **from verified comment author**, and line 121 labels
the method `github_issue_comment_verified_author` — while `author` comes from `comment`, a
**caller-supplied Python dict**, and nothing in this path contacts GitHub, verifies a signature, or
checks provenance. The module docstring itself says the comment JSON *is data only* (line 21), so the
code contradicts its own stated contract. A caller who passes `{"author": {"login": "<allowlisted>"}}`
obtains a context asserting verified GitHub provenance that was never verified. Being on the allowlist
is an authorisation check, not an authentication one.

## Evidence matrix

| # | claim | status | basis |
|---|---|---|---|
| 1 | `task_guard.validate()` builds `TrustedDispatchContext` from `task.author` on two fallback paths | **CONFIRMED** | static read at 8f40870a, lines 563/583-589 |
| 2 | that context carries `auth_method="test_fixture"`, an accepted method | **CONFIRMED** | lines 81, 569, 589 |
| 3 | `ACTOR_MISMATCH` cannot fire without a supplied context | **CONFIRMED** | line 514-516 |
| 4 | `task_envelope.admit()` accepts `comment.author.login` as verified provenance | **CONFIRMED** | lines 76-121 |
| 5 | **executable CLI negative tests** (forged author + protected op + temp test-only ledger DB; forged JSON `comment.author`) | **NOT YET EXECUTED** | see below |
| 6 | benign-fixture and mocked-authenticated controls | **NOT YET EXECUTED** | see below |

## Honest status: P0 remains OPEN

Rows 5 and 6 are the acceptance the mission actually requires, and I am **not** marking them passed:
a static read proves the code says what the report says, but it does not prove the denial codes a
running CLI emits. Per the instruction, a control with no safe executable path is marked **blocked**
rather than fabricated into a PASS.

Next stage, in this order: (a) run the forged-author CLI case against a **temporary test-only**
ledger/approval DB and record the exact denial code; (b) run the forged-`comment.author` envelope case;
(c) once Antigravity posts the fix SHA, re-run both and compare denial codes; (d) add the two controls
(benign fixture admitted; mocked authenticated actor with a correct approval admitted once). Any
admission path must not reach a real privileged executor.

## Network half of the mission — recorded as acceptance criteria for the coming fixture

Not yet checkable, because the fixture does not exist. When it does, each new host-networked test
ingress/MCP listener will be checked for: binds **`127.0.0.1` only**; **not privileged**; **no RW
key/PKI mounts**; source-owned listener distinguished from unrelated host sockets, with **fd
attribution marked UNVERIFIED when the namespace is root-owned**; and ports avoiding **18443/18444/18445**
which are still held. I will not start, stop or restart another lane's container and will not handle keys.
