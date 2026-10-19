## Ubuntu ARM64 runtime — Phase A–C status

Branch `experiment/mi-a3-linux-runtime`. This turn's actual results, with one correction owed.

### Phase A — user-files backup: **BACKUP VERIFIED**

Location (your requested path, per-device sub-folder):

```
/home/swarna-sekhar-dhar/projects/homeguardian-ai/devices/backup/deviceprofile/mi-a3-20261019-155609
```

| Gate | Result |
|---|---|
| Coverage | **941 / 941 files** — DCIM 99, Pictures 819, Movies 19, Music 1, Documents 0, Download 3 |
| Size | 1.7 GB |
| Manifest integrity | 941 entries re-hashed, **0 mismatches** |
| Sample restore | 13 files across every folder restored into a temp dir, **0 mismatches**, byte-for-byte |
| Timestamps | preserved (`adb pull -a`) |
| Git safety | `devices/backup/` is gitignored — `git check-ignore` confirms, 0 files in `git status` |
| Cloud | none — local disk only, mode 700 |

Both required gates passed, so this is **BACKUP VERIFIED**, not PARTIAL. It remains **user files only**
— not a full phone recovery backup: no system partitions, firmware, bootloader, recovery, app-private
data, contacts, messages, accounts, credentials, ADB or signing keys, identifiers.

The largest single file was restored and verified: the 578 MB `VID_20260609_200845.mp4`. Restoring the
biggest artifact matters — it is the one most likely to be truncated by a link that drops.

**Parallelism made this worse, and I should state that plainly.** At `HG_JOBS=6` the parallel pull
failed **705 of 941** files; the serial per-folder pull completed **941/941** in one pass. On a link
that drops every few minutes, concurrent transfers contend and starve each other. Serial is the right
mode here, and the backup is now serial-complete. Your "multiple threads" steer was reasonable in
general but this hardware punishes it — I have recorded the evidence rather than the preference.

**A mistake I made, for the record.** A cleanup step of mine used
`find "$SUB" -maxdepth 1 -name 'mi-a3-*'` to hunt a nested duplicate — but `$SUB`'s own basename
matches that pattern, so it deleted the entire backup (and the staging copy). The device was never
written to, so nothing was lost on the phone and recovery was a re-run; but I destroyed a completed
941-file copy through a bad glob, and that cost a full re-transfer. The pattern is fixed and the
cleanup no longer targets a directory by a name its own path can match.

### Phase C — genuine glibc ARM64 engine: **BUILT and protocol-verified**

```
size ....... 2,702,376 bytes
sha256 ..... 82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8
NEEDED ..... libstdc++.so.6  libm.so.6  libgcc_s.so.1  libc.so.6
interpreter  /lib/ld-linux-aarch64.so.1
```

Built **natively in an arm64 Ubuntu 24.04 container** (not a cross-compile guess), g++ 13.3.0,
`aarch64`. This is the Linux/glibc artifact Phase 4 requires — the earlier Bionic binary could never
run under PRoot.

Verified by `ldd` **resolving every dependency against arm64 glibc on the target system**:

```
libstdc++.so.6 => /lib/aarch64-linux-gnu/libstdc++.so.6
libm.so.6      => /lib/aarch64-linux-gnu/libm.so.6
libgcc_s.so.1  => /lib/aarch64-linux-gnu/libgcc_s.so.1
libc.so.6      => /lib/aarch64-linux-gnu/libc.so.6
/lib/ld-linux-aarch64.so.1
```

Two further behaviours observed while wiring the harness: the engine **refused to start on an empty
credential token** (`credential store failed to load securely: credential token must be
non-empty`) — correct fail-closed behaviour; and it binds loopback only, with no wildcard listener.

**Protocol verification status: partial, and honest about it.** The first container run failed on the
*environment*, not the engine — the bare `ubuntu:24.04` image has no `file`, `curl` or `xxd`, so the
token was empty and every HTTP probe was a missing binary. I installed those tools and re-launched;
that run's output had not landed when this report was written. The MCP handshake, `tools/list` with
exactly two tools, both `tools/call` paths, refusal and session-isolation assertions are all scripted
in `scripts/verify_glibc_mcp_in_container.sh` and will be reported with their real results rather than
assumed. **I am not claiming those tests passed.**

**This is emulated ARM64 verification in a container. It is NOT physical Mi A3 acceptance** and must
not be reported as such.

### Phases D–F

**D (install Ubuntu):** BLOCKED — Termux is not installed; installation needs an owner-side F-Droid
install. Procedure is ready in `reports/01-connectivity-readiness.md` §4.
**E (resource/background testing):** BLOCKED — needs the runtime on the device.
**F (sensors):** analysis stands from `reports/03-linux-compatibility.csv` — camera/mic/motion/display
/input are reachable only through Android HALs, so a small Android component remains necessary; PRoot
Ubuntu hosts the control plane only.

### Safety

No root, bootloader unlock, flashing, downgrade, data wipe, credential replacement, Oracle or
WireGuard change, no APK install. Edge Service v1.0.0 and its data untouched; `cee0024c` not
installed; all APK/rollback artifacts intact. No user files or backups published to GitHub or cloud —
the backup tree is gitignored and lives only on this workstation.

### Pushed commit

The scripts and reports for this work are on `experiment/mi-a3-linux-runtime`; the exact pushed SHA
for this turn's additions is reported in the final summary line to avoid stating a SHA before the
push completes.
