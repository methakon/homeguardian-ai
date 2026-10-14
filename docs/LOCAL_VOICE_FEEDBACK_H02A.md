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
