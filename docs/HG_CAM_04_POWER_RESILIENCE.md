# HG-CAM-04 Phase 3 — Power-Failure Resilience, Battery and Thermal Risk

Governing instruction: *"Electricity can fail without warning. Design every operation around
this risk."* Every step below is shaped by that, not by speed.

Scope note: nothing in this document installs anything, starts capture, or performs
maintenance. It records how the operations are designed and what risks remain.

## 1. Battery and power state — measured

| property | value | reading |
|---|---|---|
| level | 100 % | at full charge |
| status | `2` | **charging** |
| health | `2` | **GOOD** |
| present | true | battery detected |
| voltage | 4390 mV | normal for full charge |
| temperature | 340 (= **34.0 °C**) | warm but not alarming |
| AC powered | **false** | not on mains |
| USB powered | **true** | **drawing from the USB data connection** |
| max charging voltage | 5 000 000 µV | 5 V, standard USB |
| charge counter | 2 946 000 µAh | cumulative |

**The honest reading:** the phone is sitting at 100 % while powered over USB. It is not on
mains, and it is not on its own battery either. That is the least useful state for the
question "does it run on its own power?" — the battery is neither being drawn down nor
truly idle. Health is GOOD and the temperature is unremarkable, so nothing here blocks the
next step, but the standing rule that **continuous unattended charging at 100 % is not
approved** still applies: a full battery held on charge is exactly the condition that rule
was written for.

**The consequence for the next step:** the account removal must be a *short, attended*
operation (which is what Phase 1 specifies anyway), not a long soak. Nothing in this plan
requires the device to be left charging unattended.

**What this does not establish:** that the device runs reliably on battery alone. The only
way to know is to unplug it and watch — a deliberate, attended test that has not been run
and is not required before the account removal.

## 2. How the operations are made failure-tolerant

**Installation is atomic, so an interrupted install is safe.** `adb install` either
completes or it does not; an interruption mid-transfer leaves the previously installed
version untouched. There is no half-installed state to repair. Re-running the same install
is therefore the recovery, and no special handling is needed.

**Re-running is idempotent.** The build is reproducible and the install is re-runnable. If
power is lost, the correct response is simply to re-run the same step; there is no partial
state to clean up first. This is why the plan uses short, self-contained steps rather than
one long procedure.

**State is verified by read-back before being acted on, never assumed.** Every checkpoint
re-reads the device — the account list, the installed package version, the service state —
and reports what it actually found. If power failed during a previous attempt, the next
attempt begins by reading the device's real state, so a resumed session cannot act on a
stale belief about where it got to. This is the single most important property for
recoverability, and it is why the plan never says "the install succeeded, so now we..." —
it says "read the device, then decide".

**No long-running maintenance.** The account removal is a handful of taps. The install is
seconds. There is no step in this plan that runs for minutes and would be dangerous to have
interrupted.

**No unnecessary reboots.** A reboot costs time, is the moment when an interruption is most
disruptive, and is not needed by any step in this plan. A reboot is required only if the
owner later chooses to complete a factory reset, which is a separate, separately authorised
decision.

## 3. Ordering that keeps each step reversible

The order matters for failure tolerance: **account removal first, factory reset when and if
the owner authorises it.** Doing it in this order means an interruption during account
removal leaves a working phone with an account removed (harmless), rather than a phone
mid-reset. If power fails during a reset there is nothing to resume — the device will
simply be partway through, and the recovery is stock recovery. This ordering is deliberate.

**Explicitly not undertaken, in any phase:** bootloader unlocking, partition flashing,
factory reset, OS replacement. These are destructive, unrecoverable on failure, and none of
them is required by the CCTV goal.

## 4. Thermal risk

**Readable:** battery temperature 34.0 °C via `dumpsys battery`.

**Not readable, and this is a real limitation:** every thermal zone under
`/sys/class/thermal/*/temp` is root-only on this device, so the SoC temperature cannot be
observed. The only thermal signal available without root is the battery, which lags the SoC
badly — a hot SoC shows up late in battery temperature.

**The honest risk statement:** when the CCTV service is eventually running with the camera
and encoder active, the SoC will work harder than anything measured so far, and **we will
not be able to see its temperature.** Mitigation must therefore be behavioural rather than
telemetry-based: the device should not be enclosed, the service should stream rather than
store, and the first capture session must be attended rather than left running overnight.
Anyone who claims to be monitoring the SoC temperature on this device without root is
mistaken.

## 5. Deep sleep — the finding that shapes the CCTV design

Measured on the sibling Mi A3 node and directly relevant here: **a foreground service is not
a wake lock.** It exempts an app from Android 9's *background* restriction, which is exactly
what we need, but it does not stop the SoC suspending. Handler-based timers are
uptime-based, so they stop advancing while the device is asleep.

Two design consequences:

1. `CctvService` reports **both** uptime and elapsed realtime in its notification, so a
   suspension is visible rather than hidden. If those diverge, the device slept and the loop
   was frozen.
2. For genuinely continuous capture the node will need a **partial wake lock**. That is a
   battery-lifetime decision, not a mechanical one, and it is deliberately **not** taken
   here. It belongs with the media-test approval, because it changes the power profile of
   the device materially.

This is also why the screen-off acceptance test (spec criterion 3) is the gate for the whole
workstream: it is the test that will reveal whether the foreground service alone is enough
on this specific device.

## 6. Remaining risks, stated plainly

| risk | severity | status |
|---|---|---|
| Cannot read SoC temperature (root-only thermal zones) | medium | **no fix without root**; mitigated behaviourally |
| Continuous capture may need a partial wake lock | medium | undecided by design; needs media-test approval |
| Battery remains at 100 % on USB | low | attended operations only; no unattended charging |
| `/data` only 975 MB free | medium | design constraint: stream, do not store |
| ~464 MB swap in use | low | implies memory pressure under load |
| Screen-off survival not yet proven on this device | **high** | the next real test; nothing else matters until it passes |
| Single-disk copy of the backup | **high** | needs the owner's off-machine second copy |

The two high-severity items are both outstanding and both are honest gaps rather than
guessed-passed results. Neither is addressed by anything in this document.
