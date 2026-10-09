# HomeGuardian AI — Orchestration

## Overview

This document describes the verified Hermes-to-Antigravity delegation workflow
for HomeGuardian AI development.

## Antigravity CLI

**Binary:** `agy` (Anti Gravity CLI)
**Location:** `/home/swarna-sekhar-dhar/.local/bin/agy`
**Version:** 1.3.2
**State directory:** `~/.gemini/antigravity-cli/`

## Fixed Session

**Session ID:** `c4dccafa-6c7a-4991-a27b-e43f83530972`

This is the only session used for HomeGuardian implementation tasks.
No new session will be created without explicit authorization.

## Verified Capabilities

### Sending a Task
```bash
agy --conversation c4dccafa-6c7a-4991-a27b-e43f83530972 \
    -p "<task description>" \
    --mode accept-edits \
    --dangerously-skip-permissions \
    --model gemini-3.1-pro-high \
    --print-timeout 780s \
    --output-format text
```

### Retrieving Response
The CLI prints the agent's response to stdout. With `--output-format text`,
the response is plain text. With `--output-format json`, structured data
is returned.

### Continuing the Session
The same `--conversation` flag resumes the session with full context of
previous interactions. Tasks are sent sequentially — the agent maintains
conversation history.

### Investigation Mode
```bash
agy --conversation c4dccafa-6c7a-4991-a27b-e43f83530972 \
    -p "<investigation query>" \
    --mode plan \
    --output-format text
```

Use `--mode plan` for read-only investigation (no file edits).
Use `--mode accept-edits` for implementation tasks.

### Completion Verification
Hermes verifies all Antigravity output by:
1. Reading the response text
2. Checking `git diff` for expected changes
3. Running builds and tests independently
4. Reviewing code quality and architecture

**Never report Antigravity work as verified based on its own claims.**

## Division of Responsibilities

| Responsibility | Hermes | Antigravity |
|---------------|--------|-------------|
| Architecture decisions | Yes | No |
| Task decomposition | Yes | No |
| Coding tasks | No | Yes |
| Running builds/tests | Yes | No |
| Git operations | Yes | No |
| Code review | Yes | No |
| Verification | Yes | No |
| Documentation oversight | Yes | No |
| Commits | Yes | No |

## Filesystem Access

Antigravity operates on the same filesystem and Git working tree as Hermes.
The working directory is always set to the project root:
`/home/swarna-sekhar-dhar/projects/homeguardian-ai`

## Parallel Delegation

Parallel delegation is **not used** for HomeGuardian. All Antigravity tasks
are sent sequentially to the fixed session to prevent conflicting filesystem
or Git operations.

## Failure Handling

| Failure | Response |
|---------|----------|
| Session unavailable | Report the error; do not create a new session |
| Agent returns unexpected output | Review manually; send corrective task |
| Agent fails to complete task | Re-send with more specific instructions |
| Agent makes unauthorized changes | Revert via git; re-send with explicit file list |
| Timeout | Increase `--print-timeout`; re-send |

## Security Rules

1. `agy` must never run `git` commands — all git operations stay with Hermes
2. `agy` must never run tests or builds — Hermes verifies everything
3. `agy` receives explicit file lists — no speculative file creation
4. `agy` output is always treated as untrusted input
5. Sensitive paths and credentials are never included in `agy` prompts

## Session Verification

The session was verified on 2026-10-09 with:
```bash
agy --conversation c4dccafa-6c7a-4991-a27b-e43f83530972 \
    -p "Reply with exactly: SESSION_OK" \
    --mode plan --print-timeout 60s --output-format text
```

Response: `SESSION_OK`
