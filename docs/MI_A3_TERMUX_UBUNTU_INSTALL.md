# On-phone installation procedure — Termux → PRoot Ubuntu → MCP engine

Headless Ubuntu 24.04 LTS Minimal ARM64 inside Termux, running the verified glibc MCP engine.
Written for the owner to follow on the phone. No PC, no USB and no ADB required at any point — which
matters because the USB link is unreliable.

Legend: **[OWNER]** = you must tap or type on the phone. **[AUTO]** = a command you paste.
Anything that fails should be reported with its exact output rather than worked around.

---

## Step 0 — Before you start

| Check | Why |
|---|---|
| Battery ≥ 40%, ideally charging | the build/install is CPU-heavy |
| Wi-Fi connected | F-Droid needs network |
| ~2 GB free storage | Ubuntu rootfs plus the engine |
| `Unknown sources` allowed for F-Droid | Android will otherwise refuse the install |

Nothing here roots the phone, unlocks the bootloader, flashes firmware or touches partitions. Stock
Android 11 stays installed and the existing HomeGuardian app is untouched.

---

## Step 1 — Install Termux **[OWNER]**

**Install from F-Droid, not the Play Store.** The Play build is deprecated and **cannot install
packages** — it will fail at `pkg install` with an error about repositories.

1. Open F-Droid: `https://f-droid.org/packages/com.termux/`
2. Allow "install unknown apps" for your browser when prompted
3. Install, then open Termux once

**Expected:** a terminal prompt ending in `$`.

**Verify — [AUTO]:**

```bash
echo "$PREFIX"
# Expected: /data/data/com.termux/files/usr
# If this prints nothing useful, you have the Play build. Uninstall it and use F-Droid.
```

---

## Step 2 — Update Termux and install PRoot-Distro **[AUTO]**

```bash
pkg update -y && pkg upgrade -y
pkg install -y proot-distro
```

**Expected:** both finish without an `Unable to locate package` error.

**Verify:**

```bash
proot-distro --version
# Expected: proot-distro 4.x  (any 4.x is fine)
proot-distro list | head -20
# Expected: a table including "ubuntu" with an aarch64 note.
# Confirm the image name is exactly `ubuntu` before installing. Do not guess it.
```

If `proot-distro list` shows no Ubuntu entry, stop and report it — do not install an image from any
other source.

---

## Step 3 — Install Ubuntu 24.04 ARM64 **[AUTO]**

```bash
proot-distro install ubuntu
```

**Expected:** downloads ~100–200 MB and finishes with an installation summary. Takes a few minutes.

**Enter it:**

```bash
proot-distro login ubuntu
```

**Expected:** the prompt changes (usually `root@localhost:~#`).

**Verify — this is the architecture gate:**

```bash
uname -m
# Expected: aarch64        <-- if this is anything else, STOP
cat /etc/os-release | head -3
# Expected: Ubuntu 24.04.x LTS
df -h /
# Expected: a few hundred MB used, ~1.5 GB+ available
```

**Do not continue unless `uname -m` says `aarch64`.**

---

## Step 4 — Install the runtime libraries **[AUTO]**

Only the five shared libraries the binary needs. No desktop, no VNC, no Kali tools — deliberately.

```bash
apt update
apt install -y libstdc++6 libgcc-s1 libc6
```

**Expected:** normally all three are already present in the base image.

---

## Step 5 — Transfer the engine **[OWNER + AUTO]**

The binary must reach the phone by a local method. Pick one:

**Option A — download from the repository (simplest).** From inside PRoot:

```bash
cd /root
# Fetch the engine from the project's deploy directory (path on the branch
# experiment/mi-a3-linux-runtime: deploy/linux_arm64/hg_mcp_server)
```

**Option B — side-load from the phone's own storage** if you copied it over USB or from a cloud
drive. From Termux (outside PRoot), note the path, then inside PRoot it appears under
`/data/data/com.termux/files/home/...`.

Whichever you use, the SHA-256 **must** match:

```
82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8
```

**Verify — this gate is mandatory [AUTO]:**

```bash
sha256sum hg_mcp_server
# Expected: 82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8
chmod +x hg_mcp_server
```

If the hash differs, **stop**. A mismatched binary must not be run.

---

## Step 6 — Confirm libraries resolve **[AUTO]**

```bash
ldd ./hg_mcp_server
```

**Expected — five lines, every one resolved:**

```
libstdc++.so.6 => /lib/aarch64-linux-gnu/libstdc++.so.6
libm.so.6      => /lib/aarch64-linux-gnu/libm.so.6
libgcc_s.so.1  => /lib/aarch64-linux-gnu/libgcc_s.so.1
libc.so.6      => /lib/aarch64-linux-gnu/libc.so.6
/lib/ld-linux-aarch64.so.1
```

Any `not found` is a blocker — report it rather than installing more packages at random.

---

## Step 7 — Generate a local credential **[AUTO]**

Test credentials only. **Never copy production credentials, tokens or the existing app's identity
into this environment.**

```bash
TOK=$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')
printf '{ "%s": { "principal": "phone-test", "allow_status": true, "allow_alert_summary": true } }\n' "$TOK" > creds.json
chmod 600 creds.json
ls -l creds.json
# Expected: -rw------- (600). The supervisor refuses anything looser.
```

---

## Step 8 — Start the engine **[AUTO]**

```bash
mkdir -p run
HG_MCP_PORT=8790 HG_MCP_BIND=127.0.0.1 HG_MCP_ALLOW_REMOTE=0 \
  ./hg_mcp_server homeguardian.demo.json creds.json > run/engine.log 2>&1 &
sleep 3
grep -o 'listening on [^ ]*' run/engine.log
# Expected: listening on 127.0.0.1:8790
```

Loopback only. There is no option in this procedure that exposes it on a network interface.

---

## Step 9 — Verify the MCP surface **[AUTO]**

```bash
U=http://127.0.0.1:8790/mcp
H=(-H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
   -H 'Accept: application/json, text/event-stream')

# initialize — note the session id in the response headers
curl -sD h.txt -X POST "$U" "${H[@]}" -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"phone","version":"1"}}}'
# Expected: {"serverInfo":{"name":"homeguardian-mcp","version":"1.0.0"},"protocolVersion":"2025-11-25",...}

SID=$(grep -i '^Mcp-Session-Id:' h.txt | tr -d '\r' | awk '{print $2}')

# exactly two tools
curl -s -X POST "$U" "${H[@]}" -H "Mcp-Session-Id: $SID" -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
# Expected: get_homeguardian_status and get_alert_summary — exactly two

# read-only calls
curl -s -X POST "$U" "${H[@]}" -H "Mcp-Session-Id: $SID" -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":{}}}'
# Expected: {"capture_enabled":...,"read_only":true,"state":"Running"}

# refusal
curl -s -o /dev/null -w '%{http_code}\n' -X POST "$U" -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' -d '{"jsonrpc":"2.0","id":9,"method":"tools/list"}'
# Expected: 401
```

---

## Step 10 — Measure the real device **[AUTO]**

```bash
# memory and CPU of the engine
cat /proc/$(pgrep -f hg_mcp_server | head -1)/status | grep -E 'VmRSS|Threads'
# startup time (stop, then time a start)
# MCP latency: time 20 sequential tools/call requests
# battery and thermal (outside PRoot): dumpsys battery
```

Report these raw — they are the numbers the emulated container cannot supply.

---

## Step 11 — Screen-off survival **[OWNER + AUTO]**

Press the power button to turn the screen off, leave it 10 minutes, then:

```bash
pgrep -f hg_mcp_server && echo SURVIVED || echo KILLED
```

**Expected honestly:** Android may kill this. Rootless Termux has no init and no foreground service,
so Doze or memory pressure can end the session. If it is killed, that is a **finding, not a bug to
paper over** — and it is the reason a small Android component may still be needed for persistence.

---

## Known limitations to carry forward

1. **No init, no service manager.** `supervisor.sh` can restart the engine while the Termux session
   lives, but it cannot survive Android killing the session.
2. **No camera, microphone or motion sensors** from PRoot. Those stay Android-side.
3. **Battery optimisation** may need exemption for the Termux session to last; that is an owner
   decision, not something to enable silently.
4. **`proot-distro` adds syscall overhead.** Expect higher latency than a native process.
