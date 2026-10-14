# H-02A - Local Voice Feedback Through the Bluetooth Speaker

Status of each physical claim is stated explicitly. A process exit code of zero is NEVER treated
as evidence that a sentence was audible.

## 1. What was built

A spoken-response layer over the EXISTING single audio-output subsystem. No second audio stack
was introduced: the decision policy is pure, and the one output adapter is the same
`LocalSpeaker` that already played the operator's test tone.

    src/voice/VoicePhrasebook.{h,cpp}        closed spoken vocabulary + announcement policy (pure)
    src/voice/IVoiceOutput.h                 the seam between "what to say" and "how to play it"
    src/voice/VoiceResponseController.{h,cpp} policy + output + the feedback-suppression window
    src/backend/linux/LocalSpeaker.{h,cpp}   extended: engine selection, mute, status, probes

Flow (each step is the real component from H-01, not a parallel one):

    microphone PCM -> BoundedFeatures -> AudioActivitySession finding -> VoicePhrasebook decision
      -> LocalSpeaker (fork+execvp, bounded queue, worker thread) -> PipeWire sink -> speaker

## 2. Engine selection, version and licensing

The engine is chosen from what is INSTALLED, preferring the smallest direct synthesizer:

| order | program | how it speaks | role |
|---|---|---|---|
| 1 | `espeak-ng` | synthesizes by itself, no daemon | preferred |
| 2 | `espeak` | same, older | fallback |
| 3 | `spd-say` | needs Speech Dispatcher AND an output module | last resort, and only reported usable when a synthesizer exists to give it a voice |

Measured on this host:

* `espeak-ng` binary: **NOT INSTALLED**.
* `espeak-ng-data` (the voice data): **INSTALLED**, version `1.51+dfsg-12build1`, 13M	/usr/lib/x86_64-linux-gnu/espeak-ng-data, path `/usr/lib/x86_64-linux-gnu/espeak-ng-data`.
* `spd-say` (speech-dispatcher) `spd-say 0.12.0-rc2`: installed, but **0 output modules** are present in
  `/usr/lib/x86_64-linux-gnu/speech-dispatcher-modules/`, so it accepts a request, exits 0 and
  produces silence. That is why it is reported UNAVAILABLE rather than called a speech engine -
  this project refuses to treat exit 0 as speech.
* Candidate package for the binary: `Installed: (none)   Candidate: 1.51+dfsg-12build1` (n/a).

Licensing: `espeak-ng` and its data are **GPL-3.0+**; `speech-dispatcher` is GPL-2.0+/LGPL-2.1+.
Both are used as SEPARATE PROGRAMS invoked at run time through `fork`+`execvp` - nothing is
linked, no source is copied into this project, and no GPL code is redistributed by it. The
project's own licence stays Apache-2.0.

## 3. Installation status and the exact owner action required

Installing the binary needs administrative rights, and this milestone does not bypass operator
authentication. **The single command needed is:**

    sudo apt-get install -y espeak-ng

Nothing else is required (the voice data is already present), and no credential, password
automation, cloud SDK, Python environment or model download is involved.

Until that runs: the interface is complete and tested, and physical speech acceptance is
**BLOCKED pending installation** - stated as a blocker, never as a pass.

## 4. The spoken vocabulary (closed, explainable)

The set is fixed at compile time and a test asserts its size, so it cannot silently grow into a
claim the features cannot support.

| situation (real finding) | what is said |
|---|---|
| monitoring started | "HomeGuardian monitoring is active." |
| monitoring stopped | "HomeGuardian monitoring has stopped." |
| no samples arrived from the microphone | "Microphone monitoring has stopped." |
| output device connected | "Bluetooth speaker connected." |
| a rise above this room's own baseline | "An increase in surrounding sound activity was detected." |
| a sustained rise | "Sustained sound activity has been detected." |
| unexplained activity, statistically unusual | "Unusual sound activity was detected. The source has not been identified." |
| samples arrived but cannot be classified | "The current sound observation is inconclusive." |
| authenticated camera device present | "Camera one is connected over Wi-Fi." |
| that device went away | "Camera one has disconnected." |
| input device unusable | "Microphone input is unavailable." |

There is deliberately NO sentence for a species, an animal, a person, an intrusion, a fall, a
fire or an earthquake. `voice_text_is_forbidden()` is a data-driven check over every emitted
sentence, and a test asserts (a) no message contains such a word, and (b) the check itself really
fires on examples - so the assertion cannot pass vacuously.

A camera statement requires an EXPLICIT `authenticated_device_present` argument. A synthetic
test event cannot make HomeGuardian say a camera is connected; a test asserts exactly that.

## 5. Delivery policy

* **Confidence gate** - an observation below the threshold is not announced.
* **Minimum interval** between ANY two announcements (default 4 s).
* **Per-message cooldown** (default 45 s) so a sustained condition does not become a monologue;
  state changes are exempt from the cooldown (news stays news) and honour only the interval.
* **Bounded utterance** - operator text is sanitized (length bound, printable characters only,
  control characters refused).
* **Bounded queue** with the oldest job dropped and COUNTED.
* **A decision is not a commitment.** The policy records nothing until a sentence is really
  handed to the output, so a muted, engine-less or saturated attempt cannot silently burn the
  announcement slot. (The D-03.2 lesson - never record a fact before the work it records
  succeeded - and it was a test that caught it here.)

## 6. Feedback-loop control (suppression, NOT echo cancellation)

The microphone can hear our own speaker. The mechanism is deliberately conservative and
deliberately bounded:

1. Handing a sentence to the output opens a **bounded suppression window** (default 1.5 s).
2. Findings inside the window are not announced; the event is counted as suppressed feedback.
3. The window EXPIRES on its own, and `note_playback_finished()` can only shorten it - never
   extend it, never disable anything.
4. Environmental monitoring therefore resumes by itself. Suppression is kept strictly distinct
   from a stalled or failed microphone, which is reported as its own fact.
5. Independently, the output refuses to play at all while the microphone is live unless the
   operator explicitly allows it, and a per-message cooldown prevents repeated announcements
   during sustained sound.

**Limitation, stated plainly:** this is feedback SUPPRESSION, not acoustic echo cancellation.
There is no AEC, no reference signal subtraction and no guarantee that a sentence played at high
volume is fully ignored; the window is time-based, and a sufficiently loud/long announcement
could still be measured as activity after the window closes.

## 7. Operator controls

Listening and speaking are INDEPENDENT: the microphone has its own consent flag, and speech has
its own enable and mute.

    hg_audio_bridge --voice-status                  # engine, device, availability, last spoken, last failure
    hg_audio_bridge --speak-test                    # speak one fixed test sentence through the sink
    hg_audio_bridge --voice --mute                  # run with the microphone, speaker muted
    hg_audio_bridge --voice --no-voice              # explicit: capture without announcements
    hg_audio_bridge --voice --voice-rate 150 --voice-volume 0.3
    hg_audio_bridge --start --i-understand-this-opens-the-microphone --voice --seconds 60

Stopping the microphone (`Ctrl-C`, or omitting `--start`) leaves speech enabled; muting speech
(`--mute`) does not touch microphone consent. Extraction of raw audio is unchanged: no raw audio
is written to disk, and nothing is sent to a cloud service.

## 8. Measurements

| what | value |
|---|---|
| `espeak-ng` binary size | NOT MEASURABLE YET (not installed) |
| `espeak-ng-data` size (already present) | 13M	/usr/lib/x86_64-linux-gnu/espeak-ng-data |
| first-utterance latency | NOT MEASURED YET (blocked on the engine install) |
| subsequent-utterance latency | NOT MEASURED YET (blocked on the engine install) |
| memory during synthesis | NOT MEASURED YET (blocked on the engine install) |
| queue depth | bounded (default 8); saturation is refused and counted - asserted by test |
| suppressed feedback events | counted by the policy; asserted by test |

CPU/RAM figures for the hub are unaffected by this milestone (no hub change). Numbers that
cannot be measured yet are marked as such rather than estimated.

## 9. Physical acceptance

| stage | status |
|---|---|
| 1. offline engine synthesizes audio | **BLOCKED** - the engine binary needs the operator's `apt-get` approval |
| 2. Bluetooth playback of the test sentence | **BLOCKED** - same dependency; audibility is an operator judgement, never an exit code |
| 3. controlled real sound -> finding -> speech | **BLOCKED** - same dependency (the capture half is proven: see H-01 section 12) |
| 4. negative tests (silence, below-threshold, duplicates, saturation, mute, consent withdrawal) | **PASS as unit/behavioural tests** (17 cases) |
| 5. regression | **PASS** - see STATUS.md |

`BLUETOOTH AUDIO OUTPUT: PASS - OPERATOR CONFIRMED` is recorded for the operator's test TONE
(H-01). The spoken sentence has NOT been confirmed audible, because it has not yet been played.

## 10. What this is not

* Not Amazon Alexa. The local engine names itself honestly (`engine_name()`), and no Alexa
  branding appears in this code. Genuine Alexa runtime: **NOT AVAILABLE / NOT VERIFIED**.
* Not a live camera/CCTV viewer. The transport carries observations, not a video stream.
* Not a cloud speech service: no text or audio leaves the machine.

## 11. H-02B - corrections, real synthesis, measured cost

The engine is now INSTALLED (`espeak-ng` 1.51, `/usr/bin/espeak-ng`, 27,656 B, 132 voices, data
13 MB, GPL-3.0+ as a separate invoked program). Four defects found in H-02A were fixed, and each
has a regression test:

1. **`--speak-test` cancelled a sentence before it started.** The wait loop tested
   `speaker.busy()`, which is FALSE between the enqueue and the worker picking the job up, so it
   returned instantly and `speaker.stop()` then killed a job that had not begun.
2. **No deterministic completion.** Fixed with a monotone per-job completion index:
   `enqueue_tracked()` returns an index, `completion_marker()` is the highest FINISHED index (a
   job dropped for capacity counts as finished, so a waiter can never hang), and
   `wait_for_completion(index, ms)` is a BOUNDED wait. The seam carries it, so any output
   implementation must provide it.
3. **`engine_name()` named the wrong program** - hardcoded to `speech-dispatcher(spd-say)` even
   when `espeak-ng` had been selected. It now reports the engine that would really run, or
   `(none available)`.
4. **Zero playback can never be success.** The test path requires the completion index to be
   reached AND the played counter to have increased; otherwise it exits non-zero and says the
   sentence was not played, whatever other exit status was involved.

### Measured (fresh out-of-source Release build, `--speak-test`, sink YO-457BT)

| what | value |
|---|---|
| utterance wall time, process start to playback complete | **3.79 s** (twice, first and repeat) |
| peak RSS of the whole process tree | **7,512 kB** (7.3 MB) |
| CPU during synthesis + playback | 2 % of one core |
| engine reported | `espeak-ng` |
| `completed_within_wait` / `playback_jobs` | `true` / 1 (before 0), refused 0, last_exit 0 |

The espeak-ng binary is 27,656 B with 13 MB of voice data already installed; there is no Python
environment, model download or cloud dependency. `spd-say` remains reported UNAVAILABLE (no
output modules) and is never selected while `espeak-ng` is present.

### Real microphone -> detection -> speech

Two authorized sessions (explicit `--start` plus the acknowledgement flag, sink YO-457BT), each
with a controlled stimulus: a locally generated 3 s noise burst played by a SEPARATE tool
(`pw-cat`), i.e. acoustic coupling in the same room, not an independent environmental event.

Session A (45 s): 601 windows, 2,097,152 frames, findings `quiet_baseline`,
`insufficient_evidence`, `sustained_increase`, `sustained_decrease` - all `real_source: true` -
and `voice: spoken=2 suppressed=24 refused=0 suppressed_input_windows=150 engine=espeak-ng`.
Session B (32 s): 428 windows, 1,441,792 frames, and the transcript line

    voice_spoke: "Unusual sound activity was detected. The source has not been identified."

produced by a real `sustained_increase` (confidence 0.8, `real_source: true`). That line is the
PHRASEBOOK's closed-vocabulary sentence, which is deliberately not the same text as the
dashboard sentence in the finding JSON ("HomeGuardian detected an increase in ambient sound
activity. The source is not identified."). Both are shown, and neither names a source.

No `microphone_stalled` finding appeared in either session: the suppression window never got
misreported as a failed device, which is exactly what the separate state is for.

**Measured limitation, reported rather than hidden:** the input ring dropped 62,208 frames (3.0 %)
in session A and 26,304 (1.8 %) in session B while the analysis and speech path were busy at
48 kHz mono. The ring is bounded by design and every dropped frame is COUNTED - this is
back-pressure made visible, not a silent loss - but a 48 kHz session with speech enabled is close
to this host's capacity, and that is the honest number.

### Feed-back suppression in practice

75-150 input windows were skipped while HomeGuardian's own sentence was playing, and 10-24
candidate announcements were held back by the per-message cooldown during sustained sound. Our own
announcement never appeared as an independent finding. This remains suppression, NOT acoustic echo
cancellation (section 6).

### Operator confirmation

`OPERATOR CONFIRMED AUDIBLE` is recorded for the operator's own direct `espeak-ng` command through
the same Bluetooth sink. HomeGuardian's own spoken sentence has been produced, completed and
counted (`playback_jobs=1`, rc 0) but **is NOT marked physically verified until the operator
reports hearing it**.
