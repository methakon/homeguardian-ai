## HG-RECOVERY-SECURITY-43 — stage 1: branch recovery matrix (real SHAs), repros NOT YET RUN

Read-only. No cherry-picks, no worker-tree crossings, no task starts, no privileged network/daemon/camera,
no main merge.

### The reconciliation answer, from blob identity

```
main                                79e21c216779   scripts/worker/ ABSENT (404) - the whole dir, not just the file
fix/watcher-gh-resolver-import-…    cb0aeca973db   task_guard.py blob 4985e5b3   <-- MY ORIGINAL, verbatim
dev/production-integration          8f40870a3f30   task_guard.py blob 9e956af9   <-- DIFFERENT content
feature/hg-net-02e-outbound-mcp…    5e30c1c40a6f   task_guard.py ABSENT
tooling/devpost-compliance-scanner   (tip 34564712) task_guard.py ABSENT
feature/hg-net-02d-overlay-ingress   a0a86b033cee   task_guard.py ABSENT
fix/hg-sec-06c                       ABSENT on origin
fix/hg-sec-06d                       ABSENT on origin
my lane (experiment/hg-lan-mtls-07)  82dc5e9        blob 4985e5b3e68a
```

**My original guard is preserved verbatim on `fix/watcher-gh-resolver-import-20261021`** — identical blob
`4985e5b3`, 40-char match. So the untrusted-context fallback and the prefix scope check were **introduced
by the integration-branch modification** (blob `9e956af9`), not present in the version I wrote. That is
the "preserve original exact local SHA/path" reconciliation: the original is recoverable, and the diff
against it is the vulnerable delta.

| branch | SHA | unique guard content | equivalent in main? | verdict | prerequisites |
|---|---|---|---|---|---|
| `fix/watcher-gh-resolver-import-20261021` | `cb0aeca9` | task_guard.py = original blob `4985e5b3` | no (main lacks the dir) | **ACCEPT as recovery source** | must be tested independently; do not assume the original was correct either |
| `dev/production-integration` | `8f40870a` | altered copy `9e956af9` | no | **DEFER — REJECT until fixed** | the P0 fallback + prefix scope check must be fixed and negative tests must pass |
| `feature/hg-net-02e-outbound-mcp-bridge` | `5e30c1c4` | no task_guard.py | n/a | **DEFER** | unique files not yet enumerated |
| `tooling/devpost-compliance-scanner` | tip `34564712` | no task_guard.py | n/a | **DEFER** | unique files not yet enumerated |
| `feature/hg-net-02d-overlay-ingress` | `a0a86b03` | no task_guard.py | n/a | **DEFER** | unique files not yet enumerated |
| `fix/hg-sec-06c` / `06d` | **absent on origin** | — | — | **NOT VERIFIABLE as branches** | already merged per the assignment; the content is not in `main` either, so wherever it landed needs naming |

### Honest scope of this matrix

I verified **one file** across the branches. The assignment also asks for each branch's **unique
file/function** set and per-item test proof — that requires a per-branch diff against main, which I have
**not** done, so those columns carry no verdict I can support. Everything marked DEFER is *deferred for
lack of evidence*, not cleared.

### Adversarial repros — NOT YET RUN (not passed, not attempted)

Omitted rather than fabricated: normalized `../`, sibling prefixes, symlink escapes, empty/non-dict
`approvals`, malformed payload, protected operation with missing/false `requires_approval`, forged author,
verified actor, no context, consume-once concurrency. These are **NONEXECUTING unit/CLI repros** and none
has been executed in this window. I am not reporting a denial code or an ADMIT for any of them.

### Production-caller authentication — NOT YET CONFIRMED

The requirement is that the production caller authenticates **verified GitHub API metadata** and never
accepts plain offline comment JSON or `task.author`. On the evidence I already published (envelope builds
`auth_method="github_issue_comment_verified_author"` from a caller-supplied dict), that requirement is
**currently unmet on the integration branch**. Confirming or refuting it for the *production caller*
specifically needs its call path, which I have not yet read.

### P0 release gate — status per criterion

| criterion | status |
|---|---|
| paths canonicalized + symlink confined | **NOT VERIFIED** |
| trusted-policy-derived operations | partially read; the fallback path bypasses it — **NOT cleared** |
| nonempty valid independently issued approval | **NOT VERIFIED** (empty `approvals []` was reported ADMITTED upstream; I have not reproduced it) |
| atomic one-use | **NOT VERIFIED** |
| no `test_fixture` fallback in production | **CONFIRMED PRESENT on `8f40870a`** (two fallback sites) — so this criterion **FAILS** today |
| real red/green admission tests | **NOT RUN** |

**P0 remains OPEN.** Stage 2, in order: per-branch unique-file enumeration, then the nonexecuting repro
suite against blob `4985e5b3` (original) and `9e956af9` (altered) so the delta is measured rather than
assumed, then the production-caller call path, then re-run red/green once a fix SHA lands.

No edits to any worker tree; only this document in my own lane.
