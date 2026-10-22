# HomeGuardian AI audit — 22 October 2026 (IST)

Scope: checkout of main at `79e21c216779fe3f9e3c3d3c33df2bc6d27e5a44`, reproducibility, judge-facing claims, and available evidence. This is not a certification of hackathon eligibility or a complete security review.

## Current verdict

Native development and local synthetic processing are working. The judge release gate passes. A reproducible native HTTP transport defect remains. Live Devpost pages, current rules, and Google Drive artifacts could not be inspected: the environment proxy returned `403 Forbidden` for all four attempted page requests. Therefore video availability/duration, rendered submission fields, current requirements, and Drive contents are **unverified**, not missing by assumption.

## Verified results

| Check | Result |
|---|---|
| Release compilation | Five targets built: homeguardian, homeguardian_tests, hg_alexa_bridge, hg_mcp_server, hg_device_registry_cli |
| CTest | 3/3 checks passed |
| Native test executable | 776 cases, 76,054 assertions passed |
| Python evidence-validator unit tests | 17 passed |
| Evidence provenance | 88 artifacts verified: 5 release-contained, 83 external; 9 external citations correctly disclosed; zero unchecked cross-references |
| Alexa/bridge/HTTP/web Node tests | 77 passed, zero skipped |
| Judge verification | 27 gates passed; includes 73 native MCP protocol checks |
| MCP client | Standalone rerun exit 0; 26 checks passed |
| Dashboard/WebRTC/security | Rerun exit 0; 161 checks passed with umask 022 |
| Native functional request | Startup helper, health, operator login, CSRF and unusual scenario succeeded; 1 warning delivered/persisted, 9 duplicates suppressed |
| Unified three-tab gateway | 9/9 end-to-end checks passed, including authenticated inventory; interactive startup also verified |
| Repeat installation | Complete saved install script rerun successfully; locked npm installs and incremental builds succeeded |

No new sanitizer build, Android hardware acceptance, real-camera detection, deployed Amazon identity, remote WAN endpoint, or production cloud integration was validated. Historical documents' results are not fresh evidence from this audit.

## Prioritized gaps and work completed

### P1 — Native server rejects valid fragmented POST requests (confirmed repository defect)

`src/bridge/NativeHttp.h` listener receive loop stops as soon as `\r\n\r\n` arrives (around line 586), then dispatches the request. Dispatch requires the received body size to match Content-Length (around line 374). TCP permits headers and body to arrive in separate reads; the server sends HTTP 400 before the declared body arrives. This reproduced with Python urllib and deterministically with a raw socket. It can affect login and scenario execution, despite unit and judge gates passing.

A standalone reproducer is included as `reproduce_fragmented_post.py`. Start the native demo, then run `python3 docs/audits/2026-10-22/reproduce_fragmented_post.py <port>`. It detects a response sent before the declared body arrives. The cloud smoke check separately verified the complete-request positive path.

Required code work: receive the declared body within existing bounded size/time limits before dispatch; continue rejecting conflicting/duplicate framing headers, oversize bodies, partial bodies and pipelining. Add a real-socket test that sends headers and body separately. A complete single-send request is useful diagnostic evidence but is not a production repair.

No tracked code was changed: the invoked onboarding skill explicitly requires application source and tests to remain unchanged and directs required code work to a separate coding task.

### P1 — Live submission and artifact audit blocked by network policy

All supplied sites returned proxy tunnel 403. Saved custom domain additions: `devpost.com`, `amazonappdev2026.devpost.com`, `drive.google.com`; existing package-manager presets preserved. These draft changes do not apply themselves.

Next action: review/save these changes in environment settings and publish the environment as the UI requires. Once connectivity is applied, retry the actual URLs, inspect the video and Drive index, compare the rendered submission with current official rules, and verify public judge access. Do not assume a Drive folder is an acceptable replacement for any required public video URL. Private Drive access may require additional authorization, but this audit has not reached that access check yet.

The repository records an October 23, 2026 noon PDT deadline (October 24, 00:30 IST). This date has **not** been checked against the live rules during this audit.

### P2 — Default demo starts without the required operator secret (setup gap filled)

Plain `bash demo.sh` does not provision the launch secret required for authenticated scenario execution. With no supplied secret, `/api/session` is disabled. A secure wrapper now creates an owner-only private secret file and uses the supported `HG_SECRET_FILE` override. This was tested by delivering/persisting a synthetic alert. Wrapper: `/workspace/homeguardian-setup/start-demo.sh`. Secret values were never printed or saved in draft configuration.

### P2 — Judge-facing documentation contradicts implementation (corrections prepared)

1. `docs/SUBMISSION_PACKAGE.md` ends by listing “MCP server” among features not claimed, despite describing the real native MCP server elsewhere. Corrected copy removes that contradiction.
2. `docs/DEVPOST_DEMONSTRATION_GUIDE.md` says CMake >=3.20; CMakeLists.txt requires 3.28.3. Corrected copy states 3.28.3.
3. The same guide says first npm installation needs no cloud access; it downloads registry dependencies. Corrected copy uses npm ci and accurately describes registry access.
4. Its broad `pkill -f` advice can stop unrelated processes. Corrected copy directs users to stop only their own process.
5. `docs/DEVPOST_SUBMISSION.md` calls the demo “voice- and text-driven”; inspection found text routing and no browser speech-recognition/getUserMedia integration in the demo sources. Corrected copy says text-driven and distinguishes fresh test evidence from historical results.
6. Published native counts of 527 / 38,929 are historical. The current main checkout passes 776 / 76,054. Use the exact SHA and test scope when updating the submission; do not represent a main test run as a rerun of a different release commit.

Proposed wording corrections are included in `submission-docs.patch` for review. Existing submission documents and application code were not changed. No live submission was edited or published.

### P2 — Checkout lacks evidence refs and test build paths (setup gaps filled)

Initially the evidence validator reported 88 unresolved references. Existing Git authentication worked. Fetching the two named release/experiment branches resolved every finding without changing HEAD or the working tree. Installation instructions now repeat that fetch.

Node suites hardcode `build/`, while the documented native build uses `hg_build/`. Generated an ignored `build/` directory with links to the five binaries, preserving one build cache.

### P2 — Permission test depends on umask (setup gap filled)

Default umask 077 turned a test-created intended 0644 file into 0600, causing the negative permission test to fail and a later pairing to abort. Running the dashboard test subprocess with umask 022 restores the intended fixture. The full suite then passed 161 checks. Actual application secrets remain explicitly owner-only. Do not label this initial result as a proven authentication bypass.

### P3 — Test process interaction

The first MCP client invocation reported 26 passes but exited 1 while another verification process was active; a standalone rerun exited 0. The first run is not counted as successful. Its exact nonzero exit cause is unresolved. Saved startup instructions run state-sharing client/dashboard suites sequentially. Logs preserve both outcomes.

## Saved environment configuration

Saved complete `install_script` and `start_skill`, plus the three audit domain additions. Install instructions pin CMake 3.28.3 in a workspace virtual environment, use both npm lockfiles, fetch evidence refs, configure Release with tests and Android backend off, build with four jobs, and provide ignored compatibility links. Startup instructions cover retained dependencies versus restarted processes, secret-file setup, functional readiness checks, suite working directories, and the native HTTP defect.

Draft saving is confirmed; runtime policy changes, publication and restoration in a new task have not been verified. This commit adds audit artifacts only. Existing repository files were unchanged during onboarding. Raw logs remain local at `/workspace/homeguardian-audit`; they are excluded from Git because some test output contains session identifiers. Workspace helper paths in this report describe the audited cloud instance and are not portable repository entry points.
