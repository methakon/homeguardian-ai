# H-01 — Real hardware activation and Alexa camera readiness

Status: **H-01a partially complete (microphone and speaker discovered; the Android phone is
NOT visible to the system), H-01b/c/d/e software complete and waiting for the operator's
explicit start action, H-01f BLOCKED (no supported Amazon runtime exists).**

Branch: `feature/h01-hardware-activation`, forked from the verified D-03.2 tip
`e7029242af53ec2a53574e2af1808a2eeee13dc4`.

---

## 1. What the machine actually has (non-capturing discovery)

`uname -a`: Linux SS-DHAR 7.0.0-38-generic #38~24.04.4-Ubuntu SMP PREEMPT_DYNAMIC x86_64.
`/etc/os-release`: Ubuntu 24.04.5 LTS (Noble Numbat).

| Layer | Present | Detail |
|-------|---------|--------|
| Sound server | PipeWire 1.0.5 | exposes the PulseAudio compatibility socket (`pactl` reports "PulseAudio (on PipeWire 1.0.5)"); the `pulseaudio` binary itself is NOT installed |
| Tools | `pactl`, `wpctl`, `pw-cli`, `pipewire`, `arecord`, `aplay`, `amixer`, `alsactl` | all present |
| `pw-record` / `parec` / `pw-cat` | present | this is what the capture and playback adapters drive |
| ALSA development headers | **ABSENT** | `/usr/include/alsa/asoundlib.h` missing, `pkg-config alsa` fails, `sudo` requires a password, so a libasound-linked backend cannot be built on this host |
| TTS | `spd-say` present, **no voice backend** (`espeak-ng`/`espeak` absent) | so "spoken" output cannot be produced here; the adapter reports that instead of silently doing nothing |

Default **input** (`pactl list short sources`): `alsa_input.pci-0000_00_1b.0.analog-stereo`,
native format `s32le 2ch 48000Hz`, mapped to the built-in HDA Intel PCH `ALC897 Analog`
(codec on card 0). `arecord -l` lists card 0 device 0 (`ALC897 Analog`) and device 2
(`ALC897 Alt Analog`).

Default **output** (`pactl list short sinks` / `wpctl status`): **`YO-457BT`** — a Bluetooth
sink (bluez5, `bluez_output.7A_62_D1_27_06_FA.1`), volume 0.38. Also available:
`alsa_output.pci-0000_00_1b.0.iec958-stereo` (built-in digital) and the NVIDIA HDMI outputs
from `aplay -l` (card 1).

`lsusb`: root hubs, Intel hubs, two Bluetooth adapters (`0a12:0001` CSR, `413c:301b` Dell),
`03f0:0859 HP, Inc W100` (which is also the V4L2 video device `W100`), and a USB keyboard.
**No Android phone appears on the USB bus.**

`adb devices -l`: the daemon started, and the device list is **EMPTY**.

### What this means

* Microphone: **present and usable** (built-in analog input, 48 kHz).
* Speaker: **present and usable** (Bluetooth `YO-457BT` is the default sink; the built-in
  IEC958 output is an alternative).
* Android phone: **NOT DETECTED.** The milestone states a phone is connected by USB; the
  system reports none. Reported as a blocker, not worked around.

---

## 2. The design decision: audio is reached from the host, never from a container

The lowest-privilege viable arrangement, and the one implemented:

```
microphone ──┐
             ├── hg_audio_bridge (HOST process, the operator's own uid, PipeWire access)
speaker ─────┘        │
                      │  bounded scalars only: rms, peak, zero-crossing rate, baseline,
                      │  ratio, sustained-change flag, provenance, uncertainty
                      ▼
              observations over the existing D-03 mTLS device protocol
                      │
                      ▼
              Docker hub (NO /dev mount, NO sound-server socket, NO --privileged,
                          NO docker.sock, NO home directory)
```

Rejected alternatives and why:

| Option | Why rejected |
|--------|--------------|
| `--privileged` hub container | Grants far more than audio; the network-facing process would gain device and kernel reach |
| Mount `/dev/snd` into the hub | Raw device access for the internet-facing component; any bug in the ingress becomes a microphone bug |
| Mount the PipeWire/PulseAudio socket | The container could capture at any moment, outside the operator's model of "the mic is on"; consent would become advisory |
| Run the whole thing as one privileged process | Merges the security-sensitive network ingress with hardware access — the milestone explicitly forbids this |

The bridge is small, has no network listener, and its only output is scalars plus (on the
operator's command) speech. The hub's guarantee is unchanged: it cannot reach audio hardware
even if it is fully compromised.

---

## 3. Consent and "no silent activation"

Three independent gates, all of which must pass:

1. **Process gate** — `hg_audio_bridge` refuses to open the microphone unless `--start` is
   *and* the confirmation flag `--i-understand-this-opens-the-microphone` is passed
   (verified: exit code 3, nothing opened). `--probe` and `--help` open nothing.
2. **Adapter gate** — `PwAudioCaptureDevice::start()` throws unless
   `authorize_capture(true)` was called; constructing or initializing a device is not
   authorization, and the OS permitting the process to reach a sound server is not
   authorization either (verified in `[audio][capture][safety]`).
3. **Delivery gate** — the existing `ConsentGuardedDevice` / `ConsentGate` chain is the
   single place authorization is decided; the session does not re-check it, so there is one
   answer, not two.

Withdrawal is immediate: `authorize_capture(false)` stops the child, closes the pipe and
clears the ring, so no sample captured under the previous grant can be read afterwards.
A visible indicator is printed while capture is active and again when it stops.

---

## 4. What is implemented

| Component | File | What it does |
|-----------|------|--------------|
| Real microphone backend | `src/backend/linux/PwAudioCaptureDevice.{h,cpp}` | `IMediaDevice` audio device driving `pw-record`/`parec` as a child process: bounded PCM ring, device selection, disconnect detection (child EOF → `Error` + reason), immediate revocation, dry-run command preview |
| Real speaker backend | `src/backend/linux/LocalSpeaker.{h,cpp}` | Bounded output queue with oldest-drop, volume limit (hard clamp, NaN → 0), cancel/interrupt, locally GENERATED tone (no dependency, no download), optional `spd-say` when a voice exists, honest engine naming, feedback guard |
| Activity session | `src/audio/AudioActivitySession.{h,cpp}` | PCM → `BoundedFeatures` → `ActivityChangeDetector` (baseline/sustained) + `AnomalyMonitor` (z-score) → one explainable finding with provenance, confidence and a plain statement of uncertainty |
| Host service | `src/hg_audio_bridge.cpp` | Operator control, the two gates above, the capture loop, JSON findings on stdout, optional spoken feedback |

The finding vocabulary is CLOSED and scanned by a test: no sentence this component can emit
contains a species, an emotion, a person, or a disaster word. The allowed sentences are:

* "Learning the room's sound baseline; no conclusion yet."
* "Ambient activity is within the established baseline."
* "HomeGuardian detected an increase in ambient sound activity. The source is not identified."
* "A sustained reduction in sound activity was detected."
* "The microphone stopped delivering samples."
* "The available evidence is insufficient to classify the sound."

A stalled microphone is reported as **the device failed**, never as a quiet room — the
single most misleading thing this component could say.

---

## 5. Operator procedure (the explicit start action)

```bash
# 1. See the devices. Captures nothing.
hg_build/hg_audio_bridge --probe

# 2. Hear a short locally generated tone (output only, ~400 ms, quiet).
hg_build/hg_audio_bridge --test-tone

# 3. Explicitly authorized capture for 30 seconds, findings as JSON lines.
hg_build/hg_audio_bridge --start --i-understand-this-opens-the-microphone --seconds 30

# 4. As above, and speak the finding afterwards through the speaker.
hg_build/hg_audio_bridge --start --i-understand-this-opens-the-microphone --seconds 30 --speak
```

Steps 2 and 3 are the operator's actions. Nothing in this milestone's build or test run
opens the microphone or plays a sound by itself: the whole automated suite is silent and
deaf (`[audio]` tests use dry-run or refusal paths only).

---

## 6. Blockers

1. **Android phone: NOT DETECTED** (`adb devices -l` empty, no phone on `lsusb`). The
   operator must confirm the cable, the USB debugging switch and the on-phone authorization
   dialog before anything else can proceed. Installation, permissions, real frames and real
   PCM from the phone are all blocked on this.
2. **No voice for spoken output.** `spd-say` exists but no `espeak-ng`/`espeak` backend is
   installed, so there is no spoken message; only the generated tone works. Installing a
   voice needs a package install (`sudo` requires a password here) or a model download
   (which needs a licensing/resource review first). The adapter refuses and says why rather
   than reporting a success with no audio.
3. **No ALSA development headers**, so the capture backend drives PipeWire's CLI rather than
   linking `libasound`. This is a deliberate consequence of the no-root constraint, not a
   design preference.
4. **`GENUINE AMAZON ALEXA DOCKER RUNTIME: BLOCKED — NO VERIFIED SUPPORTED RUNTIME`**
   (section 8).

---

## 7. Docker audio boundary — what the hub can and cannot do

The hub container's reach is unchanged by this milestone: it publishes only the ingress port,
runs as uid 10001, drops all capabilities, mounts no device, no sound socket, no Docker
socket and no home directory, and it still receives only the D-03 observations. The audio
path adds **no** container privilege; that is the whole point of putting the bridge on the
host.

## 8. Amazon Alexa status

Checked, and the honest answer is blocked:

* Docker images on this host: `homeguardian-hub:local`, `homeguardian-ai:*`,
  `hgdev:compile`, `debian:trixie-slim` — **no Amazon/Alexa image**, and none is expected:
  Amazon publishes no official Alexa Built-in container image.
* Amazon AVS developer tooling is no longer generally available for new Alexa Built-in
  integrations; Alexa+ capabilities are partner-only. This was established during D-01 with
  HTTP status evidence and has not changed.
* The existing HomeGuardian `alexa_skill/` prototype is a Node test surface for a
  **fictional/simulated** Alexa+ web demo. It is not an Amazon runtime and is not treated as
  one.

Therefore, exactly as instructed:

    GENUINE AMAZON ALEXA DOCKER RUNTIME: BLOCKED — NO VERIFIED SUPPORTED RUNTIME

Nothing in this milestone claims Alexa, uses Alexa branding, or presents the simulated
dashboard as Amazon integration. The phone can still become a functional HomeGuardian Wi-Fi
camera independently of that external milestone (section 9).

## 9. The Alexa-enabled Wi-Fi camera path (investigation only)

For a camera to be usable by Amazon's smart-home skill interfaces
(`Alexa.CameraStreamController`, `Alexa.RTCSessionController`) it must be a **certified,
account-linked device**: that requires an Amazon developer account with the camera
capabilities enabled, device certification, an account-linking endpoint, and an
authenticated video session negotiated through Amazon's own infrastructure. None of that is
available locally, and installing this APK on a phone does not create it. Recorded as an
external blocker requiring Amazon account approval, not as work that can be finished here.

**What is NOT done, and will not be done:** exposing an unauthenticated RTSP or HTTP camera
stream on the LAN, or presenting the phone as "Works with Alexa" because the APK is
installed.

## 10. First physical capture attempt and what it proved (and did not)

An explicitly authorized capture session was run against the wired microphone
(`alsa_input.pci-0000_00_1b.0.analog-stereo`, verified NOT to be an output `.monitor`).

**Verified real:**
* the adapter opened the physical input and received **32,768 real PCM samples** from it;
* features computed from those samples were non-zero and real: `rms 0.0682 / 0.0621`,
  `peak 0.1367 / 0.1101`, with `provenance: real` and `windows_observed` advancing;
* the finding vocabulary behaved: `warming_up` first, then `insufficient_evidence`;
* capture stopped cleanly on the session's own stall detection and released the device.

**Defects found and fixed by this attempt** (all in the committed source):
1. `pw-record` has no `--raw` option; the adapter passed it, the child exited instantly with
   "unrecognized option", and the session reported a stalled microphone. The argv builder is
   now tool-aware (`parec --raw --format=s16le --device=…` vs
   `pw-record --format=s16 … --target=… -`), and `parec` is the default because it is proven.
2. The child's stderr was silenced, hiding that one-line message. It is now captured into a
   bounded buffer and reported with the exit status.
3. A read waited only 20 ms and the stall threshold was three reads, so a tool's normal
   ~0.2-1 s startup latency was reported as a hardware fault. The first read after start now
   waits `first_read_grace_ms` (2000) and later reads `read_wait_ms` (200), bounded.
4. An output `.monitor` passed as a microphone is now REFUSED at construction: monitoring the
   desktop's own playback is not room sound, and it would feed the speaker's announcements
   back into detection.
5. The child's stderr pipe is now drained continuously (bounded), so a diagnostic pipe cannot
   fill and block the data path.

**NOT yet working — stated plainly: a sustained capture session stops after one pipe buffer.**
The session delivers exactly 65,536 bytes (32,768 samples), then `read()` returns EAGAIN
indefinitely while the child stays alive (exit status 0, no EOF, no `disconnects`, nothing on
stderr). The same command run manually streams **589,824 bytes in 5 s**, so the tool and the
device are fine: the difference is in how this harness consumes the child. This is an
unresolved limitation of the host bridge, not a property of the microphone, and it is the
immediate next diagnostic step.

## 11. Android status: still not detectable

`adb devices -l` is empty after an `adb kill-server` / `start-server` cycle, and the full USB
tree contains no phone vendor ID at all (only Intel hubs, a CSR8510 and a Dell Bluetooth
receiver, the HP W100 camera and a USB keyboard). A phone *is* present in the room, but over
**Bluetooth** (an "S25 Ultra" is paired at `00:00:46:88:4C:50`) - which is an audio pairing
and cannot carry ADB. Nothing was installed, and no device property was invented.

## 12. Sustained capture: root cause, and a correction to an earlier claim

**Status: PASS.** A 35 s authorized session captured **1,605,632 samples over 489 windows**
(33.4 s of audio at 48 kHz mono, i.e. realistic timing), with `dropped=0`, no stall verdicts,
a clean stop and no leaked subprocesses. The earlier ceiling was 32,768 samples.

### 12.1 Correction: the `O_NONBLOCK` explanation was wrong

An earlier report claimed that setting `O_NONBLOCK` on the adapter's read end also made the
**capture child's** writes non-blocking, because `dup2` shares the open file description. That
is wrong, and it was verified rather than argued about: a small program sets `O_NONBLOCK` on a
pipe's read end and reports both ends. The write end stays blocking (read end 0 -> 1, write end
0 -> 0). Read and write ends of a pipe are *different* open file descriptions. Only a `dup` of
the **same** end shares flags (demonstrated: `dup` of the write end, then a flag change on the
copy, is visible on the original). The claim is retracted here and does not appear in the
source comments as a rationale.

The reader-thread redesign that accompanied it is kept on its own merits - blocking I/O on a
dedicated thread with `poll()` is the right structure, and it removes the sleep-loop the
consumer used to run - but it did **not** cause the fix, and it is not described as having done
so.

### 12.2 The actual root cause: our own SIGTERM, sent on a false verdict

`strace -f` on the adapter settles it. The capture child was **alive and waiting** in its
PipeWire poll loop when it received:

```
975949 --- SIGTERM {si_code=SI_USER, si_pid=975948} ---   # 975948 is the adapter itself
```

It then shut down gracefully and exited 0 - which is exactly the "clean exit status 0" that was
mistaken for the capture tool quitting on its own. The sequence was:

1. The PipeWire graph delivered a burst, then went momentarily quiet (source suspended/being
   reconfigured) and the adapter's ring was empty.
2. The stall verdict fired after **3 empty reads (~600 ms)**.
3. The caller stopped the session, which sends SIGTERM.
4. The device was killed by us, mid-session, after 0.68 s of audio.

The diagnostic matrix that isolated it to our spawn rather than the tool: direct shell
**393,216 B / 6 s**; a minimal Python subprocess harness (which sets `close_fds=True`)
**401,408 B / 6 s, rc=0**; our adapter **32,768 B and then a stall**. The difference was never
the reading path - it was that we declared the device dead and killed it.

### 12.3 The fix

The stall bound is now a **time bound, not a retry count**: `stalled_after_empty_windows = 50`
(~10 s at the 200 ms read pace). A device that is quiet for a moment is not a broken device, and
a momentary gap in the sound-server graph is not a hardware fault. The finding's wording was
corrected with it: it now states the fact and the bound ("delivered no samples for N consecutive
windows", with the two plausible causes named) instead of asserting a diagnosis ("the microphone
stopped delivering samples").

Child-process hygiene added along the way and kept: the forked child closes inherited
descriptors above stderr before `execvp`, so it does not hold the parent's pipes or terminal.

### 12.4 What is still unproven

* The 35 s figure is one session on this machine. Sustained capture across a **device
  disconnect/reconnect** and across a sound-server restart is not demonstrated.
* Bluetooth audibility is an operator judgement, not a measurement (section 13).
* No claim is made that the graph pause itself is understood - only that pausing is normal and
  that our reaction to it was wrong.

## 13. Bluetooth audio output - OPERATOR CONFIRMED

The operator confirmed that the generated test tone was audible through the Bluetooth speaker.
This is the only evidence that counts for audibility: a playback process exiting 0 proves the
program ran, not that a human heard anything (see the `spd-say` case below, which exits 0 with NO
output modules installed and produces silence).

`BLUETOOTH AUDIO OUTPUT: PASS - OPERATOR CONFIRMED` (device `YO-457BT`, PipeWire sink
`bluez_output.7A_62_D1_27_06_FA.1`, engine `homeguardian-generated-tone`).

## 14. Spoken feedback

Implemented in H-02A - see `docs/LOCAL_VOICE_FEEDBACK_H02A.md`. Status there:
physical speech acceptance is BLOCKED pending the operator's `espeak-ng` installation.

## 15. Bluetooth spoken sentence - OPERATOR CONFIRMED (H-02B)

`Bluetooth spoken sentence: PASS - operator personally confirmed clear audible speech.`

* Output device: `YO-457BT` (PipeWire sink `bluez_output.7A_62_D1_27_06_FA.1`).
* Local speech engine: `espeak-ng 1.51`.
* Sentence: "HomeGuardian local voice feedback test is successful."
* This is genuine audible HomeGuardian LOCAL TTS. It is **not** Amazon Alexa, and no Alexa
  branding or runtime is involved.

This is OWNER-SUPPLIED acceptance evidence: the owner listened and reported clear audible speech.
It is not something this agent heard or measured itself. The programmatic counterpart (a completed
playback job, non-zero exit only on real playback, engine named honestly) is recorded in
`docs/LOCAL_VOICE_FEEDBACK_H02A.md` section 11 - a process exit code is still never treated as
evidence of audibility.

Unchanged limitation, retained: the microphone input ring dropped 1.8-3.0 % of frames during
48 kHz capture with speech active (bounded ring, every drop counted - visible back-pressure, not
silent loss).
