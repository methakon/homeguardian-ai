# Owner-authorized network camera testing procedure

**Status: not run.** This is the procedure to follow when the owner authorizes a physical session. No
probing of any V380 device has been performed, and **no standard-protocol support is assumed for it** —
V380 units are commonly RTSP/ONVIF-free and require the vendor's own application, so the procedure treats
"no compatible stream" as a likely and acceptable outcome rather than a failure to work around.

## Rules for the session

1. Owner present and authorizing, in the designated non-private area only.
2. **No credentials are entered, guessed or bypassed**, and no firmware or configuration is changed.
3. Passive discovery first (`ip neigh`, existing records, the device's own documentation). Bounded probes
   (a single TCP connect to a known service port on the owner's own device) only with the owner's explicit
   go-ahead for that specific address.
4. No stream is made public, no raw media is committed, and nothing is recorded beyond bounded scalars.
5. If a device cannot be reached or speaks a proprietary protocol, the finding is recorded as a limitation
   and the work continues against the standards-compliant producer fixture.

## Per-device record (fill one per device, no credentials)

| field | how to establish | example |
|---|---|---|
| vendor / model / firmware | label, its own UI | (owner-provided) |
| application or protocol | documentation, the app it ships with | proprietary / RTSP / ONVIF |
| RTSP availability | one bounded TCP connect to 554 | reachable / refused / not attempted |
| ONVIF availability | one bounded TCP connect to 80/8000, WS-Discovery only if the owner asks | reachable / refused |
| video encoding | device documentation, not guessed from a stream | H.264 / H.265 / MJPEG / unknown |
| audio input | device documentation | yes / no / unknown |
| audio output / talkback | device documentation; **not enabled during this procedure** | advertised / unknown |
| authentication method | documentation | app account / digest / unknown |
| stream transport | documentation or a single `rtsp://` OPTIONS if authorized | TCP interleaved / UDP / unknown |
| network reachability | passive ARP entry, then one bounded connect | reachable / absent |
| identity stability | does it present a stable certificate or device id? | stable / per-session / unknown |
| consent + authorization | owner's statement, recorded with the session | granted / withheld |

## If a compatible stream exists

Local acquisition only: the stream is reduced **on the machine that acquires it** to bounded scalars
(motion fraction, audio RMS/peak/ZCR), and only those scalars are sent as observations through the contract.
No frames leave the machine, and no frames are stored. Then run the existing harness:

```sh
hg_scenario_runner --instructions
hg_scenario_runner --only light_on_off --interactive --occupancy empty --confirmed \
    --deliberate "light on at t=8s, off at t=20s"
```

The seven-scenario operator harness (`docs/STAGED_SCENARIO_HARNESS.md`) is unchanged and remains the way
physical camera evidence is gathered, with its occupancy annotation required for a run to count as evidence.

## If no compatible stream exists

Record the exact limitation (protocol, ports refused, documentation) and continue with the
standards-compliant producer fixture: the network contract is device-agnostic, so a producer implemented
against it is the correct integration target, and a proprietary device simply does not participate until a
supported path exists. Do **not** reverse-engineer the device's protocol or credentials to force
participation.
