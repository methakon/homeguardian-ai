# AmazonAppDev 2026 Hackathon — Final Compliance Audit

**Audit date:** 2026-10-12 (IST)
**Verified HEAD:** `896f4be` on `main`, **clean working tree**, in sync with `origin/main`.
**Note on baseline:** the task stated expected baseline `7334543`; actual HEAD was `896f4be`
(F22-33 local-only HTTP adapter, committed immediately prior to this audit). That newer work
was **preserved** and audited as-is.
**Time to deadline:** ~11 days (deadline **Oct 23, 2026 12:00 PM PDT = Oct 24, 2026 12:30 AM IST**).

> [!NOTE] HISTORICAL COMPLIANCE AUDIT CONTEXT (2026-10-12)
> This document records the pre-implementation hackathon compliance audit conducted on October 12, 2026 at commit `896f4be` prior to PR #20.
> At the time of this audit, HomeGuardian AI pursued Route B (simulated Alexa+ experience).
> **Subsequent Implementation Status (PR #20 / October 2026):**
> HomeGuardian AI fully implemented and certified a genuine native C++ Model Context Protocol (MCP spec 2025-11-25) server (`hg_mcp_server`) with Streamable HTTP transport, fail-closed authentication, and 2 read-only tools. See `docs/DEVPOST_DEMONSTRATION_GUIDE.md` and `docs/PROOF_REGISTER.md` for current certified architecture.

---

## 1. Official sources inspected (accessed 2026-10-12)

| Source | Key requirements extracted |
|---|---|
| `amazonappdev2026.devpost.com/rules` + main page | Primary track + optional mini-challenge; each project wins one track prize and one mini-challenge. Repo must **actually call the track's required technology in code/import/entry point/loaded config — a README mention does not count** (Fire TV excepted). Demo video <3 min, public on YouTube/Vimeo. Product feedback for every tool/API/SDK used. Friction log can earn up to +10% judging bonus. |
| Official update "Let's get you building" + "Got an idea" | Alexa+ track: "build an **Agent Skill or a self hosted MCP server on the open MCP standard**. **No MCP experience? Simulate the Alexa+ experience with any agentic tools you already know.**" |
| Main page, Alexa+ track detail | "New to MCP? Build a **simulated Alexa+ experience in a web app using your preferred agentic tool**." And: "Took the simulated-experience path instead? **No specific framework or SDK required**, but your repo still needs to **include the simulation's source code**, and your demo needs to clearly show it working." |
| AWS Builder mini-challenge | "Use AWS services (Bedrock, AgentCore, Strands SDK, **Kiro Crew**, SageMaker, etc.) … **Building with Kiro Crew qualifies on its own**." |
| Open Source mini-challenge | A distinct qualifying public open-source contribution or new public open-source project during the window. |
| Forum thread "Private repo path" | Official clarification: `@AmazonAppDev` is a GitHub **organization and cannot be added as a collaborator**; the private-repo path requires sharing with **`testing@devpost.com`** (and Amazon reviewers via Devpost). |
| `modelcontextprotocol.io/specification/2025-11-25` | Streamable HTTP is the required transport (HTTP+SSE deprecated); MCP endpoint accepts POST; `MCP-Protocol-Version` header; servers running locally **SHOULD bind only to 127.0.0.1**; **MUST validate Origin**; SHOULD authenticate. |

**Rule amendments found:** the hackathon page deadline reads **Oct 23, 2026 @ 12:00pm PDT** (matches the
task's stated deadline). The gated Alexa+ partner tools (Category SDK, MCP Toolkit, `@alexa-ai/cli`,
Alexa+ Web Simulator) are **closed to hackathon participants** — self-hosted MCP, Agent Skill, or a
self-built web simulation are the only routes. AWS $150 promotional credit requests **closed Oct 7,
2026** — no action needed or possible.

---

## 2. Project reality check (code-verified, not doc-verified)

| Claim in docs | Reality (verified) |
|---|---|
| README: "Alexa+ conversational experience … **with MCP integration**" | **FALSE.** No MCP code exists: zero hits for `modelcontextprotocol`, `jsonrpc`, `tools/list`, `Streamable` in `src/` and `alexa_skill/`. This is a credibility risk with judges who verify claims. |
| README: "Ring integration" | **FALSE.** No Ring code. |
| README "Proposed (not yet implemented)": Boost.Beast/Drogon HTTP server | Superseded in practice: the HTTP surface is `src/bridge/hg_http_adapter.js` (Node stdlib `http`, loopback-only). README is stale here too. |
| "License to be selected" | **No LICENSE file exists.** Repo is **already PUBLIC** (`gh repo view` → `visibility: PUBLIC`). |
| C++ core (event model, consent gate, pipeline, correlation, SQLite, simulator) | **TRUE and tested** — 255 cases / 2004 assertions pass. |
| End-to-end household workflow | **TRUE and runnable** — verified live: capture-on → consent-gated synthetic observations → `Alert generated: synthetic simulator presence notice` → dedup (1 delivered / 8 suppressed) → persisted → status summary. |
| Alexa boundary (contract, credential store, stdio bridge, HTTP adapter) | **TRUE and tested** — `AlexaContract` (read-only status/alert_summary), `AlexaCredentialStore` (0600, fail-closed), `hg_alexa_bridge` (NDJSON stdio), `hg_http_adapter.js` (loopback HTTP). 68 Node tests pass. |
| Demo script | **TRUE, runs clean (exit 0)** — `docs/demo_alexa_integration.sh`. But it is **raw NDJSON JSON lines** (poor for video) and **requires a pre-built `hg_build/hg_alexa_bridge`** that a cloning judge will not have. |
| Build instructions | README says `mkdir build` — works, but FetchContent needs network **once**; no note that `hg_build` must be built and the bridge target is `hg_alexa_bridge`. |
| AWS services/SDKs | **NONE** — no `aws-sdk`, `boto3`, Bedrock, AgentCore, Strands, SageMaker, or Kiro usage or config anywhere. |
| Secrets in public repo | **CLEAN** — scanned; only comments stating "no real secrets". |

---

## 3. Route A vs Route B

### Route A — MCP / Agent Skill: **DOES NOT QUALIFY (0% implemented)**
No MCP server exists. Requirements unmet: spec 2025-11-25+, Streamable HTTP transport, `initialize`
handshake, capability negotiation, `tools/list`, `tools/call`, JSON-RPC framing, `MCP-Protocol-Version`
header, Origin validation. The stdio bridge is NDJSON-over-stdin, **not** MCP stdio (different
framing, no initialize/tools protocol); the loopback HTTP adapter is a plain REST-ish API, **not** an
MCP endpoint. **Estimated effort to a compliant minimal MCP server: 2–3 days** (JSON-RPC framing,
initialize/tools-list/tools-call, version header, Origin validation, tests) **plus a public tunnel for
judges to reach it** — the rules require a remotely accessible URL, which conflicts with this
project's no-public-listener constraint and the deadline.

### Route B — Simulated Alexa+ web experience: **PARTIALLY BUILT, FASTEST TO ELIGIBLE**
What exists: a real, tested, read-only backend (`AlexaContract` + `AlexaCredentialStore` +
`hg_alexa_bridge` / `hg_http_adapter.js`) with genuine HomeGuardian functionality behind it
(consent gate, event pipeline, correlation, alerts, dedup, persistence). What is missing: a **web
front-end that presents a conversational Alexa-like experience and visibly invokes those tools**.

### Decision: **Route B (simulated Alexa+ web experience)**
| Factor | Route A | Route B |
|---|---|---|
| Eligibility confidence | Low — 0% built, needs public tunnel | **High** — official rules explicitly allow it, "no specific framework or SDK required" |
| Existing code reuse | None | **High** — the entire backend already exists and is tested |
| Implementation effort | 2–3 days + hosting | **~1–1.5 days** (a web UI that calls the existing bridge over HTTP) |
| Integration/demo risk | High (public exposure, TLS, auth per MCP spec) | **Low** (local loopback, screen-recorded) |
| Demo quality for judges | Protocol-level, abstract | **Far better** — a visible household workflow |
| Deadline fit (11 days) | Tight | **Comfortable** |

The simulated path is also **exempt from the "must actually call the track's technology" import
requirement** — but the repo must include the simulation's source code and the video must clearly
show it working. Both are achievable.

---

## 4. Mini-challenge eligibility

### AWS Builder: **DOES NOT CURRENTLY QUALIFY**
No AWS service, SDK, or qualifying development tool is used anywhere in the code, dependencies, or
documented workflow. Kiro Crew is not used (no `.kiro`/`.amazonq` config); the documented
development workflow does not establish use of a qualifying AWS tool.
**Recommendation: do not chase this.** Adding AWS services purely to claim $5,000 introduces cost,
risk, and deadline pressure without improving the product. If a genuinely useful, free-tier-safe
integration appears during Route B work (none is needed), revisit. **Do not request AWS credits** —
the program closed Oct 7, 2026.

### Open Source: **DOES NOT CURRENTLY QUALIFY**
HomeGuardian itself is public but is the primary submission, not a distinct contribution. Reviewing
the account's public repos: all non-fork public repos are the participant's own projects
(`my-job-agent`, `option-chain-paper-trading-playbook`, `x402-algorand-endpoint`, `tantric-ai-agent`,
`chat-sync`, etc.); the rest are forks. **No verifiable upstream contribution (merged/accepted PR)
during the Aug 31–Oct 23 window was found.**
**Recommendation: optional, low priority.** A small, real, independently-scoped upstream PR
(typo/doc fix in a dependency actually used — e.g. nlohmann/json, spdlog, or Catch2) is realistically
achievable in 1–2 hours if time remains after Route B + video. It must be a genuine accepted
contribution, not a fabricated one. **Do not make any repo public or publish private source** without
explicit approval.

---

## 5. Submission compliance checklist

| Requirement | Status | Evidence / action |
|---|---|---|
| Primary track selection | **PARTIAL** | Alexa+ is correct; must be selected in the Devpost form (user action). |
| Required technology / valid alternative | **PARTIAL → PASS after P0** | Route B once the web simulation ships (its source in-repo). |
| Significant updates during window | **PASS** | 35 commits between 2026-10-09 and 2026-10-12 (all F22-xx milestones); `git log` verifiable. |
| Runnable project + accurate setup/run | **PARTIAL** | Builds and tests pass, but README build/run steps omit the bridge target and the demo depends on a prebuilt binary. Fix in P0. |
| Repository accessible to judges | **PASS** | Repo is **already PUBLIC** — no collaborator/`testing@devpost.com` sharing needed. |
| Required source, assets, deps, license, test instructions | **FAIL** | **No LICENSE file** on a public repo. Vendored deps (sqlite, Catch2) and FetchContent deps are documented but a license must be added. |
| Demo video <3 min, public YouTube/Vimeo | **UNVERIFIED** | No video exists. **User-only task.** |
| Product feedback for every tool/API/SDK used | **UNVERIFIED** | Must cover: C++20/g++, CMake, spdlog, nlohmann/json, SQLite, Catch2, Node.js, ASK SDK (if kept), MCP spec (studied). **User-only task** (draft in P1). |
| Project description / problem / functionality / novelty | **PARTIAL** | README exists but **overclaims** (MCP, Ring). Must be corrected to describe the simulator honestly. |
| Honest simulator-vs-real description | **FAIL today** | README claims MCP/Ring integration that does not exist. This is the single biggest credibility risk. |
| Open-source mini-challenge evidence | **N/A** | Not claimed (see §4). |
| AWS Builder evidence | **N/A** | Not claimed (see §4). |
| Friction log quality | **PASS (strong)** | `docs/FRICTION_LOG.md` — 27 chronological incidents with evidence, root cause, remediation, regression coverage, residual impact. Qualifies for the +10% bonus. |
| IP / third-party licensing / privacy / secrets | **PARTIAL** | Secrets clean; privacy posture strong and documented. Missing LICENSE; vendored-third-party attribution not stated. |
| Remaining form fields / submission steps | **UNVERIFIED** | Devpost submission, video upload, feedback form — all user-only. |

---

## 6. Judging-criteria assessment (4 equally weighted)

**Technical Implementation — currently ~6/10, → 8–9/10 after P0.**
Real strengths: 255 C++ test cases / 2004 assertions, ASan/UBSan clean, fresh-build reproduction,
a genuine consent gate, event pipeline, correlation rules, dedup policy, transactional SQLite
persistence with migration/retention/recovery tests, and a hardened credential store. The weakness
is that none of it is reachable through a *user-facing* interface today — judges see a CLI and raw
JSON. A web simulation makes the engineering visible.

**Design — currently ~4/10, → 8/10 after P0.**
The backend is well-structured, but there is no user-facing design at all. A clean conversational UI
showing consent state, observations, correlated alerts, and privacy controls is where the points are.

**Potential Impact — ~7/10 (story is strong, proof is thin).**
The privacy-first, consent-gated, local-first home-safety positioning is a credible, specific
customer need with a real differentiator vs cloud-dependent competitors. Must be paired with an
honest "this is a simulated, local demonstration" statement — judges penalise overclaiming, and the
honesty itself reads well.

**Quality of the Idea — ~7/10.**
Consent-first household intelligence with explainable alerts and durable audit is a genuinely
good idea, and the engineering discipline (friction log, fail-closed auth, bounded resources) is
above average. Framing it as an Alexa+ add-on surface with explicit privacy boundaries is a
creative, well-understood use of the ecosystem.

**Minimum compelling end-to-end scenario for the video (reproducible from README):**
1. Start the simulated Alexa+ web experience (one command).
2. Grant consent for the simulated living-room sensor (show the consent gate).
3. Speak/type "Is everything okay at home?" → the experience invokes the real backend.
4. Detection observations arrive → the detection/fusion rule qualifies → a detection-driven
   alert appears with its evidence (fusion verdict, fused confidence, rule, timestamp).
5. Show the dedup/cooldown behaviour on a repeat query.
6. Withdraw consent → show subsequent observations are blocked (fail-closed).
7. Show the persisted alert summary survives a restart (`historical_alerts`).
8. Close on the privacy boundary: no raw media, metadata only, local-only, no cloud.

---

## 7. Ranked action list

### P0 — Blocking for eligibility and credibility (must do, ~1.5 days total)
| # | Task | Effort | Depends on |
|---|---|---|---|
| P0-1 | **Fix README overclaims**: remove "MCP integration" and "Ring integration"; state honestly that this is a *simulated* Alexa+ experience on the official simulated-experience route, with no MCP server and no real device integration. | 30 min | — |
| P0-2 | **Add a LICENSE file** (MIT recommended) — repo is public with no license; this also unblocks any future open-source claim. | 10 min | user approval of license choice |
| P0-3 | **Build the simulated Alexa+ web experience** (`src/web/` or `alexa_skill/web/`): a static HTML/JS page served by a small Node launcher that talks to `hg_http_adapter.js` (or the bridge directly), presenting a conversational UI with consent toggle, query box, alert cards with evidence, and a status panel. Include its source in-repo. | 1 day | — |
| P0-4 | **Make the demo self-contained**: a single `demo.sh` that builds (or reuses) the bridge, generates test credentials, starts the web experience, and prints the URL — so a judge can reproduce it from a clean clone. | 2 h | P0-3 |
| P0-5 | **Update README** with the corrected one-command run instructions for the web demo + build/test commands. | 1 h | P0-3, P0-4 |

### P1 — Judging quality (do if time permits, ~0.5 day)
| # | Task | Effort |
|---|---|---|
| P1-1 | Draft the **product-feedback text** for every tool used (C++/g++, CMake, spdlog, nlohmann/json, SQLite, Catch2, Node.js, ASK SDK, MCP spec) — purpose / what worked / what needs improvement / onboarding / would-use-again. | 2 h |
| P1-2 | Write the **demo video script** (≤3 min) following the §6 scenario, and a `docs/VIDEO_SCRIPT.md`. | 2 h |
| P1-3 | Add a short `docs/HACKATHON.md` linking track choice, route justification, mini-challenge decisions, and the friction-log bonus. | 1 h |

### P2 — Optional, only after P0+P1 are done and verified (~2 h)
| # | Task | Effort |
|---|---|---|
| P2-1 | One genuine upstream open-source contribution (doc/typo fix in a dependency actually used), submitted as a real PR, for the Open Source mini-challenge. **Only if it is genuinely accepted-quality work.** | 1–2 h |
| P2-2 | Extend `docs/FRICTION_LOG.md` with a hackathon-window entry recording the README-overclaim finding and its fix (evidence of honest self-correction). | 30 min |

**Explicitly NOT recommended:** implementing a real MCP server (Route A), adding AWS services,
requesting AWS credits (closed), making any repo public, or building production TLS.

---

## 8. Deadline-driven plan (ends before Oct 24, 2026 12:30 AM IST)

| When (IST) | What |
|---|---|
| **Oct 12 (today), by EOD** | P0-1 (README honesty fix) + P0-2 (LICENSE) — commit + push. |
| **Oct 13–14** | P0-3 web simulation + P0-4 self-contained demo. Commit + push daily. |
| **Oct 15** | P0-5 README refresh; run full validation (C++ suite, Node suite, ctest, ASan/UBSan, fresh build). |
| **Oct 16–17** | P1-1 product feedback, P1-2 video script, P1-3 HACKATHON.md. |
| **Oct 18–19** | **Record the demo video** (<3 min), upload to YouTube/Vimeo (public or unlisted). |
| **Oct 20–21** | **USER: submit on Devpost** — track selection, repo URL, video URL, feedback, description. |
| **Oct 22** | Buffer: verify submission renders, video plays, repo is public and builds from a clean clone. |
| **Oct 23 (by 18:00 IST)** | **Final verification + submission buffer.** Deadline is Oct 24 00:30 IST — nothing new after 18:00 on Oct 23. |

---

## 9. Owner-only tasks
1. **Devpost submission**: select the Alexa+ track, enter description, repo URL, video URL, and product feedback.
2. **Record and upload the demo video** (<3 min) to YouTube or Vimeo.
3. **Approve the LICENSE choice** (MIT recommended) — required before P0-2.
4. Optionally accept any Devpost collaborator invitations (not needed — repo is public).

---

## 10. Final recommendation

### **CONDITIONAL GO** — submit on time **if and only if P0-1 through P0-5 are completed and verified by Oct 15.**

**Why conditional, not GO:** the project is technically strong but currently **fails two submission
requirements** — no demonstrable user-facing product for the Alexa+ track (Route B needs the web
simulation) and **no LICENSE on a public repository** — plus the README **actively overclaims**
capabilities (MCP, Ring) that do not exist. Submitting as-is risks disqualification-level problems
(no demonstrable track technology) and a serious credibility hit with judges.

**Why not NO-GO:** every blocker is small, well-scoped, and backed by already-tested backend code.
The official rules explicitly permit the simulated-experience route with no framework requirement,
the repo is already public, the friction log is submission-grade, and 11 days remain — roughly 7×
the estimated P0 effort. The engineering foundation is genuinely good; it just needs a face and an
honest description.

**Next action (immediately after this report):** implement **P0-1 (README honesty fix) and P0-2
(LICENSE, pending user approval of MIT)** as one small commit, then proceed to **P0-3 (the simulated
Alexa+ web experience)** — the single highest-value task for both eligibility and judging score.

## License options (v15, 2026-10-13) — DECISION REQUIRED, nothing committed

P0-2 (no LICENSE on the public repo) is still open. **No license has been added
or selected.** Options for the user to choose:

1. **MIT** — permissive, shortest, most common for hackathon/demo code; allows
   reuse and modification with attribution; no patent grant.
2. **Apache-2.0** — permissive like MIT **plus an express patent grant** and a
   NOTICE/trademark clause; better for anything that might see corporate use.
3. **BSD-3-Clause** — permissive, no patent grant, adds a no-endorsement clause.
4. **No license (all rights reserved)** — default copyright; code is *not*
   open-source and others cannot legally reuse it. Acceptable if the repo is to
   stay private later; public repo + no license usually means no reuse rights.

**Note:** the repository is currently **public** with **no license**, which is
the least helpful state for judges and reusers. A LICENSE is a one-line decision
but a legal one, so it is left to the user. If MIT/Apache/BSD is chosen, also
update `README.md`'s license section and, for Apache-2.0, add a `NOTICE` file.

## P0-2 RESOLVED (v16, 2026-10-13) — Apache License 2.0

**Decision:** the project owner explicitly approved **Apache License, Version
2.0**. (Recorded here so the earlier "options" list above is read as history, not
as an open question.)

**Implemented:**
- Root `LICENSE` = the complete, unmodified Apache License 2.0 text. Verified
  byte-for-byte against the official text (`https://www.apache.org/licenses/LICENSE-2.0.txt`):
  sha256 `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30` for
  both the fetched official copy and the committed file.
- Root `NOTICE` added: copyright notice + third-party attribution.
- `README.md` license section updated from MIT to Apache-2.0 with a NOTICE link.

**Attribution review (no relicensing of third-party code):**
| Component | Location | License | Action |
|---|---|---|---|
| SQLite | `third_party/sqlite` (bundled) | Public domain | preserved (own notice in source) |
| Catch2 | `third_party/catch2` (bundled) | Boost Software License 1.0 | preserved (own notice in headers) |
| spdlog | CMake FetchContent (not vendored) | MIT | attributed in NOTICE |
| nlohmann/json | CMake FetchContent (not vendored) | MIT | attributed in NOTICE |
| ask-sdk-core/model | `alexa_skill` (npm, optional) | Apache-2.0 | attributed in NOTICE |

**Copyright holder:** `Copyright 2026 Swarna Sekhar Dhar` — evidenced by the
repository owner, 45 of 46 commits, and the prior MIT copyright line. One commit
(the `NativeHttp.h` foundation, `6a996daf`) is authored by a different name; the
owner should confirm whether a second copyright line is desired. No ownership was
invented.

**Note (correction):** an earlier v15 statement said the public repo had "no
license". That was inaccurate — the repo carried an **MIT** `LICENSE` (added in
`f1a923b`). P0-2 was therefore not "missing license" but "license choice"; it is
now resolved as Apache-2.0 by explicit approval.

**P0-2 status: DONE.**

## Superseded rows in the claims audit above (2026-10-13, after L-11)

The claims table at the top of this document was authored at an earlier
milestone. The following rows are now out of date; the findings themselves are
left intact as a historical record:

| Original row | Current state |
|---|---|
| "C++ core … 255 cases / 2004 assertions" | Suite is now **329 cases / 2453 assertions** (assertions verified in this milestone's run). |
| "End-to-end household workflow … `Alert generated: synthetic simulator presence notice` → dedup (1 delivered / 8 suppressed)" | The alert is now **detection-driven** (L-11): rule `detection-fusion`, message names the fused evidence, severity derived from the fused confidence. The demo run now reports e.g. 10 generated / 1 delivered / 9 suppressed (Unusual activity) or 10 / 2 / 8 (Default, two severities). The old presence notice no longer exists anywhere in `src/`. |
| "README: Alexa+ … with MCP integration" | Corrected: the README no longer claims MCP. |
| "License to be selected / no LICENSE file" | Resolved in v16: Apache-2.0 `LICENSE` + `NOTICE`. |
| Boost.Beast/Drogon HTTP surface | Superseded: the demo HTTP surface is the native C++ transport (`hg_alexa_bridge --http`), with the Node adapter retained only as an optional legacy reference. |

No new claim is made here; every figure above was produced by this milestone's
test run or live demo.
