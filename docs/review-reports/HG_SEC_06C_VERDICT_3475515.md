# HG-SEC-06C — independent verdict for `347551558fc3c4f5deb11b2c566b86ecc6e59db4`

Sanitized verdict. Detailed findings are retained in this private review folder and the scratch evidence
paths below; nothing here is for public release.

    SHA        347551558fc3c4f5deb11b2c566b86ecc6e59db4  (branch fix/hg-sec-06c-lifecycle-auth)
    PARENT     2eb05bb (the previous FAIL candidate) - no substitution
    PROVENANCE materialised via git archive; 5 tested blobs re-verified with git hash-object against
               git rev-parse <sha>:<path> at the end of the run. CLI 1e186e66…, MCP server e20d1440…
               (bit-identical to the baseline - the MCP server is untouched by this delta)
    DELTA      4 files, +288/-50, zero compiler errors
    SCOPE      206 checks: 203 PASS, 3 FAIL, plus 6 items in the second reviewer's matrix

## VERDICT: **FAIL — NO-GO. Do not approve the release.**

The operator trust boundary is **not** independently secured, and the directive is explicit that the
release must not be approved while that is true. Both HG-SEC-06B blockers survive in reduced form.

## The trust root is a writable file in `/tmp` - the decisive finding

The expected operator credential is still derived, in order, from `HG_OPERATOR_SECRET`,
`HG_OPERATOR_PASSWORD`, then a secret file at `HG_OPERATOR_SECRET_FILE` or the **default
`/tmp/hg_operator_password.secret`** (`main_device_registry_cli.cpp:108-129`). That default anchor:

- exists with mode **0600 owned by uid 1000 - unprivileged, the same identity as the caller**;
- sits in a **world-writable `1777` directory**;
- is read with `stat` (not `lstat`), with **no ownership check**, **no `O_NOFOLLOW`**, **no `O_EXCL`**,
  and a separate `ifstream` open after the mode check;
- is written by `web_mcp/auth.js:29-31` as well.

Live-verified by the reviewer: the anchor was **overwritten and used to authorize a device recovery with
no environment variable set - exit 0, credential replaced** (anchor then restored, hash verified).
Pre-creation of the path from the caller's own account is accepted with **no warning necessity**; a
symlink to a 0600 file is accepted (target mode governs); and a **TOCTOU swap between the check and the
read is read and trusted, including a swapped-in 0644 file, exit 0**. A caller-supplied
`HG_OPERATOR_SECRET_FILE` path works the same way.

Because the *expected* credential is caller-authorable, the gate is not a gate. Confirmed consequences:

- `HG_OPERATOR_SECRET=<caller value>` + `--operator-auth pair <unprovisioned physical camera>` → **rc 0**,
  credential issued, impostor heartbeats, view token **200 "Physical camera connection authorized."**
  The same works for a fresh id and for `camera-syn-001`.
- `revoke` then `HG_OPERATOR_SECRET=<caller value> --operator-recovery` → **rc 0**, device revived.

Bare-bootstrap is closed (`env -i` and wrong-credential enrolment exit 3 with a byte-identical registry);
the hole is the anchor, not the gate's absence.

## Secondary failures

- **`--operator-secret` is still accepted, silently (rc 0, no warning)** and is visible in
  `/proc/<pid>/cmdline` (mode 0444, any local user) and `ps -eo args` for the window before the parser
  runs. The post-parse scrub does work, but it cannot close the pre-main window. It also lands in the
  operator's own shell history. The usage string no longer advertises it, yet `docs:48` still lists it -
  contradictory steering.
- **`--operator-secret-stdin` can never authorize.** `pair` verifies twice (`:654`, `:660`) with no
  caching, so the first read consumes the pipe: one line → rc 3, two lines → rc 0. Fail-closed on the
  empty second read (no empty-read-is-authorized case), so this is a usability defect, not a hole.
- **A view token minted before revocation is still accepted after revocation and recovery** - HTTP 200
  with a real SDP reply, 0.1 s after the device is healthy again. The refusal while revoked is the device
  gate, not token invalidation; `auth.js:207-228` never learns about revocation. Bounded by
  session-binding, single-use and the 60 s TTL.
- **Sequence watermark reset is inconsistent.** `DeviceRegistry::pair()` wins over the CLI's stated
  preservation (`DeviceRegistry.cpp:289` beats `:748-755`): watermark 50 → 0 after a re-pair, and a
  previously rejected sequence is then accepted. `reset_sequence` is dead code. Bounded, because the old
  credential stays dead.

## What genuinely improved (verified)

Consent, capture permission and online state **are** invalidated on re-pair/recovery, with view issuance
refused until a fresh authenticated heartbeat. Secret-file permissions are enforced on **every** path -
only 0600/0400 accepted, 0640/0644/0666 rejected in both 0700 and 0777 directories with byte-identical
registries, symlinks governed by their target's mode (27/27). All 13 crafted payloads (JSON flags,
`is_synthetic`/`device_type` overrides, caller-supplied `auth_secret` or `credential_id`, stale
credentials) were refused. Historical credentials are dead with positive controls; 0600 file / 0700
directory after every write (199 poll samples); rotation and revocation semantics hold; replay is
rejected across a process restart and a cwd change; no secret leaks into any CLI, MCP or gateway output,
logs, inspector or the audit database - `provisioned_auth_secret` appears only in the pairing response
and `rotated_auth_secret` never echoes the old value. MCP and dashboard regressions pass: 401 without a
bearer, 200 for `tools/list` and `tools/call` with one, hostile-origin 403 with no
`Access-Control-Allow-Origin` against a working genuine origin, forged `simulator` refused, `/api/pair`
still 404.

## Smallest corrective scope

1. **Anchor the trust root outside caller control.** A root-owned, non-group/world-writable file (e.g. a
   0600 `root:root` path under `/etc`, or a build-time hash); require **uid 0 ownership as well as the
   mode**; open with `O_NOFOLLOW | O_EXCL` and read from the **same file descriptor** that was checked
   (closing the stat-then-open TOCTOU); reject symlinks via `lstat`. Never read a caller-supplied path
   and never fall back to `/tmp`.
2. **Never let one source supply both values.** Environment variables may present a credential; they
   must not define the expected one.
3. **Gate initial enrolment on that same anchor**, so `--operator-auth` cannot be self-authorized.
4. **Invalidate view tokens on revocation and recovery** - bind tokens to a device generation/epoch, or
   have the token path consult revocation state.
5. **Make the watermark policy consistent**: either `pair()` must stop zeroing `last_sequence`, or the
   reset must be intentional and the stale-sequence window closed another way.
6. **Cache the operator credential after the first read** so the stdin channel works through the double
   verification.
7. **Drop `--operator-secret`, or warn loudly**, and fix the contradictory documentation line.

## Release status

**NO-GO.** The operator trust boundary is not independently secured, and the release must not be promoted
while it is not. Per the directive I am issuing one SHA-specific verdict and stopping promotion; the
submission engineer may select a later candidate, and these findings must not be attributed to any
commit other than the one named above. Note for that work: the branch has since advanced to `51f5522`
(HG-SEC-06D), which rewrites the CLI and `auth.js` - `3475515` remains a verified ancestor and every
finding above is pinned to it.

## Evidence

    verdicts   evidence/sec-01/HG-SEC-06C_PART_A_3475515.md   (items 1-5, takeovers, anchor, stale token)
               evidence/sec-01/HG-SEC-06C_PARTB_3475515.md    (206 checks: permissions, argv, leakage,
                                                              regressions, owner additions)
    scratch    ~/.hermes/cache/scratch/hg-sec06c-partA/ and ~/.hermes/cache/scratch/hgsec06c-pb/
    prior      HG-SEC-06B_FINAL_VERDICT_2eb05bb.md, HG-SEC-06_FINAL_VERDICT_05e1303.md (preserved)

No merge, no deployment, no production infrastructure change. Aliases: one build was SIGKILLed with no
compiler diagnostic while a sibling build ran (resource death, not a source break) and was rebuilt alone.
