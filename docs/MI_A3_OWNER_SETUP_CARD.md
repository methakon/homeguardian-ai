# Mi A3 — owner setup card

The minimum to get the verified MCP engine running on the phone. No PC, no USB, no ADB.
Everything here is done on the phone by hand.

The full step-by-step with expected output for every command is in
`MI_A3_TERMUX_UBUNTU_INSTALL.md`. This card is just the short path.

---

## Do these four things

**1. Install Termux from F-Droid — not the Play Store.**

Open `https://f-droid.org/packages/com.termux/` on the phone and install it.

> The Play Store build cannot install packages. It fails later at `pkg install` with a
> repository error, and looks like a network problem when it isn't. F-Droid is the only
> correct source here.

**2. In Termux, run:**

```bash
pkg update -y && pkg upgrade -y
pkg install -y proot-distro
proot-distro install ubuntu
proot-distro login ubuntu
```

**3. Inside Ubuntu, confirm the architecture:**

```bash
uname -m
```

Must print `aarch64`. If it prints anything else, stop and report it.

**4. Install the libraries and the engine, then verify:**

```bash
apt update && apt install -y libstdc++6 libgcc-s1 libc6
sha256sum hg_mcp_server
```

The hash must be exactly:

```
82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8
```

If it differs, **stop** — a mismatched binary must not be run.

Then run the bundled self-test, which does the rest and prints a verdict:

```bash
bash self_test.sh --tsv results.tsv --json results.json
```

---

## What is still needed to do the rest

The engine binary has to reach the phone. The install doc covers the transfer options; the
SHA-256 check above is the gate that matters, whichever route is used.

---

## One limitation you should know before relying on this

**A supervisor running inside Termux cannot restart itself after Android kills the entire
Termux process.**

This is not a bug and not something a better script fixes. It follows from how Android works:

- Termux is an ordinary app. It has **no init, no systemd, no service manager** — so nothing
  inside it is privileged, and nothing inside it can survive its own process being killed.
- Android may kill the whole Termux app — supervisor, engine and shell together — under
  Doze, memory pressure, or battery optimisation.

`supervisor.sh` restarts the engine **only while the Termux process is alive**. It correctly
notices a dead engine and starts a new one. But if Android kills the parent, there is no
process left to do the restarting, and the engine stays down until Termux is opened again or
something outside Termux restarts it.

**Consequence:** this setup suits intermittent and development use, not unattended
always-on operation. Durable operation needs an Android-side component — a foreground
service with a persistent notification, which Android treats as far less killable. That is
the one thing a small Android piece is still genuinely required for, and it is separate from
the MCP runtime itself.

Battery-optimisation exemption for Termux can reduce how often this happens. It does not
remove the limitation, and it is your decision to make, not something to be enabled quietly.
