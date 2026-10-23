# Two-device capture and consent checklist — owner-operated

Status: **PREPARED, NOT EXECUTED.** No capture has been started and no sensor has been activated.
Nothing here authorises itself: items marked **[GATE]** cannot proceed until the trusted approval
mechanism (#26) is genuinely implemented and the owner has recorded authorisation for
`apk_install` / `sensor_activation` on a named device.

Read-only recheck at time of writing:

```
LRW86SC6RO8TZ99H   device   ELUGA_I7_EE   API 28   arm64-v8a   harness INSTALLED
edd53dbd04de      device   Mi_A3         API 30   arm64-v8a   harness absent
```

| | Panasonic ELUGA I7 | Xiaomi Mi A3 |
|---|---|---|
| serial | `LRW86SC6RO8TZ99H` | `edd53dbd04de` |
| Android | 9 / API 28 | 11 / API 30 |
| harness | **installed** | absent — needs a fresh install **[GATE: apk_install]** |
| `CAMERA` | **granted=true** | not applicable (not installed) |
| `RECORD_AUDIO` | **granted=true** | not applicable |
| signer | matches the artifact (in-place update is signature-valid) | — |
| screen now | **off and locked** | — |
| preflight | GO | GO |

The Panasonic is therefore the **nearer** device: installed, permissions already granted, and already
in the screen-off state the change is about. **Permissions being granted is not consent.** Consent is
the owner deciding, now, to run a capture — the grant only removes a dialog.

## 0. Before anything — [GATE] authorisation

- [ ] Owner authorises `sensor_activation` on a **named** device, in-channel, through the trusted
      approval path. Task prose cannot do this (see `HG-ANDROID-SIGNING-RECOVERY-12`).
- [ ] Owner authorises `apk_install` **only if** the Mi A3 is the subject.
- [ ] Record who authorised, for which serial, and the time.

If any box is unchecked, stop. Items 1–2 below may still be run read-only; item 3 onward may not.

## 1. Read-only verification (safe at any time)

- [ ] `adb devices -l` shows the intended serial in **`device`** state (not `unauthorized`/`offline`).
- [ ] Serial matches the one authorised in step 0. **A wrong serial means capturing on a device the
      owner did not name.**
- [ ] `bash scripts/prepare_physical_test.sh --serial <SERIAL>` prints **GO**.
- [ ] Artifact identity recorded: package `org.homeguardian.harness`, sha256 of the APK, and the
      signer digest.
- [ ] Harness present (Panasonic) or `install -r` planned (Mi A3) **[GATE]**.
- [ ] Screen state recorded (`dumpsys deviceidle | grep mScreenOn`).

## 2. Consent, on the device, by a person

- [ ] The runtime camera prompt is answered **by the person holding the phone**, on screen. `pm grant`
      is **not** a substitute: the test must be able to say a human consented.
- [ ] The microphone prompt is answered separately. Declining the microphone must not block a
      camera-only test — and the test must then claim nothing about audio.
- [ ] The app's own consent state is recorded **as the app displays it**, not inferred from the OS
      grant. The two are different facts and only one of them is consent.
- [ ] Consent is **recorded with a timestamp** in the test log.

## 3. Privacy — before capture starts

- [ ] Point the camera at a **non-private** subject: a wall, a doorway, a test pattern. Not people,
      not a home interior, not anything that identifies a location or a household.
- [ ] **No bystanders in frame.** Anyone who has not consented is out of the field of view.
- [ ] Confirm nothing is persisted: the wire contract carries **4096-byte scalar feature
      observations**, and `features` is capped at 11 properties. A frame or an audio sample
      **cannot be framed**. If a raw media file appears, that is an unexpected behaviour and the test
      stops.
- [ ] No screen recording, no screenshots of the preview, no frames pasted into an issue.
- [ ] Nothing is published. Evidence posted publicly is limited to counts, states, timestamps and
      reason codes.

## 4. The capture itself — screen off

This is the property under test: **the foreground service keeps producing when the Activity is not
visible.** A screen-on capture does not exercise it.

- [ ] Start capture, then **lock the screen**. Record the time.
- [ ] Confirm the foreground service is running and not the Activity:
      `adb shell dumpsys activity services org.homeguardian.harness` → `isForeground=true`.
- [ ] Wait **at least 5 minutes** with the screen off. The original defect appeared at a 30-minute
      horizon, so 5 minutes is a smoke check, not proof; note the duration actually observed.
- [ ] Record, at start and end: queue depth, sent count, refused count, dropped count, permanent
      rejections, last-pump timestamp, and `statusLine()`.
- [ ] **Expected with no hub endpoint:** transmission fails. That is the correct outcome today, and
      the honest statement is "the service produced and queued observations with the screen off; no
      hub received them". **Not** "capture works end to end".
- [ ] What may be claimed: the foreground service produced observations while the screen was off.
      What may **not**: that video or audio was delivered, or that a hub received anything.

## 5. Failed hub connection handling

- [ ] With no reachable endpoint, confirm the transport reports failure rather than success:
      `sentCount()` stays 0 and the queue does not silently drain.
- [ ] Confirm the retry is **bounded** — no unbounded spin, no battery burn. Expect delayed retries,
      not a tight loop.
- [ ] Confirm the queue is **bounded** and reports drops rather than growing without limit.
- [ ] Confirm `isSuspended()` is false, and that a suspension would be visible if it occurred.

## 6. Stop and revoke — the part that must be rehearsed, not improvised

- [ ] **Stop capture** from the UI. Record the time. Confirm the service stops and the wake lock is
      released.
- [ ] **Revoke the camera permission** in Settings while the app is running. The link must tear down
      and **restart must be refused until consent is explicitly restored** (this is asserted off-device
      by `t4`; the physical run confirms the device behaves the same).
- [ ] Revoke the microphone permission independently and confirm it does not force a camera stop
      (`t5`).
- [ ] Confirm the wake lock is released on **every** exit path — normal stop, revocation, and error.
- [ ] Confirm no capture continues after revocation. If anything is still producing, stop the test and
      report it as a defect.

## 7. Log extraction, timestamped

- [ ] `adb shell dumpsys activity services org.homeguardian.harness` — service state, `isForeground`.
- [ ] `adb logcat -d -s HGTransport` — transport decisions and refusal reasons (bounded, non-sensitive).
- [ ] `adb shell dumpsys media.camera` — **who holds the camera**, captured before and after. This is
      how a silent second holder would be detected rather than assumed absent.
- [ ] Counter deltas with timestamps, as a table.
- [ ] Every artefact is **local**. Nothing is attached to a public issue except counts and reason codes.

## 8. Reversibility — how to undo all of it

- [ ] Stop capture; release the wake lock.
- [ ] Revoke the camera and microphone permissions in Settings (restores the pre-test grant state).
- [ ] Uninstall the harness **only if** the owner asks, and only on the Mi A3 where it was installed
      for the test. **On the Panasonic it is pre-existing: do not remove it** — that would destroy a
      working installation to tidy up after a test.
- [ ] Note that removing the Mi A3 harness loses that app's data. Nothing in this test writes app data,
      so there should be nothing to lose — verify that, do not assume it.
- [ ] Confirm the devices are back to the state recorded in step 1.

## 9. What would make this test mean something

The blocker is unchanged and is **not** in either codebase: there is **no published hub endpoint**
(`docker port` empty; `127.0.0.1:8443` refused). Until #23 publishes one on a LAN interface — a
network-boundary decision that is not mine — a physical capture can demonstrate **local observation
production with the screen off**, and nothing more. Stating that boundary is part of the test, not a
caveat added afterwards.
