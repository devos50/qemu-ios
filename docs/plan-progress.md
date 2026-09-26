# Emulator and Light Touch plan progress

The workstream follows the supplied hardware-fidelity plan, with reproducible
storage corruption promoted ahead of new peripherals. Each completed item needs
both a focused regression and relevant guest evidence. Bluetooth peers and
TV-out were explicitly deferred by that plan.

| Track | Status | Acceptance evidence still needed |
| --- | --- | --- |
| Generated NAND integrity and restart stability | Complete for the reproduced failures: free-pool bounds, FMSS completion, VIC fixes and paced TV-out IRQs | Twelve alternating Coldplay/Spore install/respring cycles; unchanged system file; guest shutdown; cold boot with both apps; full-volume read-only fsck passed |
| PMU ADC and masked event IRQ | Complete | Ten-bit results, settling vs conversion, mask/read-to-clear and GPIO tests pass; real 7E18 boot, three lock/wake cycles and native shutdown pass |
| Battery controls | Core and Light Touch level/charging/drain controls implemented | 20/60 percent cold calibration, full-voltage estimate, runtime off/on/auto and native shutdown pass; preserve guest filtering delay |
| Headset/Mikey detection | Deferred at user request (2026-09-05) | Plug/unplug and headset button traces; correct guest routing |
| Microphone/I2S RX | Deferred at user request (2026-09-05) | Deterministic input tone captured by the guest, then host microphone recording |
| Native idle sleep/wake | Untethered manual/automatic lock and Home/Power wake verified without brightness override | Deeper suspend paths still need acceptance |
| Kernel serial console | Complete: explicit machine arguments, live console regression and Light Touch control build pass | Include the updated control in the final package verification |
| Settings Wi-Fi join | Deferred at user request (2026-09-05) | Manual join without known-network seed or alert loop; DHCP and traffic |
| Two-instance LAN | Deferred at user request (2026-09-05) | Separate identities, MACs and state; bidirectional traffic between guests |
| Hardware shortcuts | FMSS completion and TV-out frame timing fixed; VIC daisy-chain defects fixed; DSI panel reply queue implemented; NOR program/erase/private persistence and operand-driven PKE verified | PKE interrupt/timing, FMSS erase, remaining LCD/TV-out status, timers 0–3 and AES signature classification |
| Light Touch integration | Original UI fixes, built-in proxy/TLS bridge, inline Live Text, capture, native logs/notices, status pane, AFC browser and private NOR storage packaged | Broader accessibility; Help Book deferred at user request |
| Help Book | Deferred at user request (2026-09-06) | Existing help window remains available |
| Media imports | Music and Saved Photos native imports, content identity and recording audio verified | Artwork/playlists/Videos, old-import indexing and reconciliation with guest Photos deletions |
| Configuration and firmware | Exact guarded 5F138/7E18 profiles; audio, keyboard, Bluetooth and watchdog options typed | Boot-chain, decode, audio/display/GLES options and remaining environment consumers |
| Bluetooth peers / TV-out | Deferred per supplied plan | No implementation claimed |

See [UI reliability](ipod-ui-reliability.md) for the original fixes and the
storage reproduction. Temporary `/tmp` paths there identify local evidence;
they are not dependencies of the committed tests.

## User priority update (2026-09-05)

Finish the frame-polling crash fix, non-quitting Power Off/On, sleep/off visuals,
and foreground-app subtitle first. Then implement HTTP proxy configuration with
WaybackProxy integration, followed by screen capture/recording, Live Text and
optional touch/finger overlays (using WireView as a reference), then resume the
remaining active plan. Headset/microphone, Settings Wi-Fi join and two-instance
LAN are deferred by explicit user request.

## Proxy and capture follow-up

- Frame-poll startup crash fixed and sanitizer-tested (`074da22b44`).
- Non-quitting Power Off/On, sleep/off visuals and foreground subtitle built
  and native power cycles verified (`460d062`, `7c3cdae801`).
- HTTP proxy: built-in direct HTTP and optional dated Internet Archive replay implemented.
  Native NSURLConnection through guestfwd returned HTTP 200 for an unresolvable
  origin using a controlled upstream fixture; reversible configd preferences
  passed on/on/off/off. Host sanitizer tests cover HTTP framing and tunnels.
  No external proxy installation is needed. Host archive acceptance returned a
  September 2009 example.com page; the subsequent native guest request received
  the archive's HTTP 429 response (rate limiting, not a local routing failure).
  Modern TLS termination remains a later phase.
- Native screenshots, inline Live Text and video recording implemented. Real
  H.264 encode/decode tests cover colors, rotation margins, dimensions and timing;
  a 68-second UI recording is playable. Guest audio recording is now verified below.
- Finger dots are 44 AppKit points, soft white/gray with shadow and fast release
  fade, suppressed asleep/off. Their layers are siblings of the framebuffer to
  isolate opacity. Sleeping uses the user's Sleeping.caar animation.

### Proxy and overlay corrections verified

- No Proxy tolerates Safari's stale proxy connections while configd applies
  settings. Archive replay follows original HTTPS redirects on the host and
  rewrites absolute HTTPS links only in text resources; binary data is unchanged.
- Archive requests share a paced process gate, Retry-After cooldown and bounded
  date/URL cache. Loopback ASan/UBSan tests cover compressed HTML, redirects,
  cache hits during cooldown, parallel 429 suppression and switching off.
  Live Archive availability is not claimed fixed; explicit HTTPS URLs still
  require a future TLS bridge.
- Release build passed. Isolated UI inspection showed the framebuffer visible
  after touch release, inline text recognition, and the animated Sleeping asset.

### Console and capture toolbar follow-up

- Routine SDIO traces require IPOD_SDIO_TRACE=1. GLES first-slot diagnostics and
  framebuffer readbacks for diagnostic color averages require IT_GLES_VERBOSE.
  Normal rendering no longer stalls on the diagnostic offscreen glReadPixels.
  GLES surface/upload regressions and native builds passed.
- guestfwd merges stderr into its socket. The HTTP helper now prevents diagnostics
  from corrupting HTTP status/framing; merged-stream failure regression passed.
  Native NSURLConnection fetched both example.com and neverssl.com with HTTP 200
  through direct proxy, then neverssl.com with HTTP 200 after disabling it.
  Evidence: /tmp/it-proxy-direct-native-v2.log. Earlier host NeverSSL TCP timeouts
  were transient. Modern HTTPS/NYT compatibility still needs the TLS bridge.
- Screenshot, Record/Stop, and Live Text are default/customizable toolbar items;
  Copy Screen and Finger Dots are also customizable. Copy Screen is Shift-Cmd-C.
  A native VisionKit fixture using the production view verified that the corner
  Stop Analyzing Image button removes the complete Live Text overlay.

### TLS bridge acceptance checkpoint

The reproducible `test_webproxy_tls_guest.py` passed on a fresh 7E18 overlay:
untrusted SHA-1 RSA chain rejected; guest-local CA added using securityd's
user-domain API; TLS 1.0/AES128-SHA HTTP 200; CA removed; chain rejected again.
The helper needs the period-correct `modify-anchor-certificates` entitlement,
not the modern prefixed name. Evidence: /tmp/it-proxy-tls-native-v4.log.
Production CONNECT termination and automatic per-device CA management remain.

### Built-in TLS bridge implemented

CONNECT now terminates legacy guest TLS locally and fetches HTTPS using verified
system libcurl. A per-device CA is generated atomically in private state; the app
installs/removes only its public certificate in the guest through securityd.
The Mac trust store and base NAND are unchanged. The helper bundles OpenSSL
statically. Native guest HTTPS returned HTTP 200 for example.com and NYT. Offline
TLS sanitizer tests cover concurrent CA initialization, trust/name validation,
HTTP errors inside TLS, and rejection of an untrusted upstream. Release app built.
This completes the TLS transport step; modern WebKit feature gaps remain.

### Guest agent transport and native operations

- Added bounded cp15 RPC queues, strict base64 framing, memory-copy error checks,
  lease recovery, host cancellation, and reset invalidation. Tokens identify
  sessions; they do not secure the tunnel against other guest processes.
- `it_agent` retains the explicit-UTI clipboard pumps and adds ping, exec, atomic
  put/get, settime, launch/frontmost/lockstatus, kill, and launchd halt requests.
  Child process groups have bounded output and a timeout; command execution
  keeps clipboard/heartbeat ticks running. Guest receive pages are touched before
  host debug writes, which cannot fault in demand-zero pages.
- Native acceptance passed binary stdin/files, root shell execution, exit status,
  host-clock correction, and Settings launch/foreground detection. Evidence:
  `/tmp/it-agent-native-v4.log`. Protocol and operation ASan/UBSan tests pass.
- Added QMP and embedded frontend APIs. Acquired references protect frontend
  calls that overlap machine cleanup. Light Touch's command dispatcher builds
  and its isolated Swift check passes concurrent result routing, cancellation,
  and absent-agent fallback. It never retries a submitted mutation through SSH.
- Baked a separate `nand-agent-v2` candidate, removed its old clipboard launch
  job, fixed daemon/job ownership, and passed HFS validation. The existing
  `nand-ultimate` remains untouched. Baked launchd/restart acceptance is ongoing.
- Remaining Track A: typing injection, UI tree inspection, ranged reads,
  snapshot-load rekeying, and completing image/frontend rollout.

### Native typing and UI inspection

- SpringBoard's DYLD_INSERT_LIBRARIES is inherited by launched apps on 7E18;
  the minimal native probe logged separate SpringBoard and Settings PIDs.
- Added a bounded foreground-process route with expiring request cookies. The
  injected library handles type/backspace/uidump on the main run loop, without
  signal overrides or temporary IPC files. Root and UI replies cannot complete
  one another's requests. A non-consuming key check avoids idle SBS IPC polling.
- Native Notes and an installed Harness UITextField passed Unicode insertion,
  deletion, physical host keys, and UI-tree/text inspection. Bulk input uses the
  focused control's insertText: API; treating a paragraph as one keyboard key
  produced an oversized autocorrection prompt and was corrected. Repeatable
  acceptance: `test_agent_guest.py --typing`, /tmp/it-typein-regression.log.
- Command-agent review artifact: `build-native14/Light Touch-agent.app`, signed
  and verified, contains the validated agent-only NAND candidate. Typing rollout
  into the baked image and existing devices is the next step.

### Guest agent rollout completed and snapshot work underway

- Baked typing/command-agent launchd recovery passed, including Notes and Harness
  Unicode, physical keys, Spotlight, discarded unfocused keys and locked input
  rejection (`/tmp/it-agent-unlock-native.log`). SpringBoard focus IPC runs on a
  worker; synchronous queries from its main thread deadlocked its own service.
- Existing-device provisioning preserves launch configuration, replaces the old
  clipboard job and starts command service immediately. A real isolated Light
  Touch upgrade passed; the second launch reported media already current.
  Release build and dispatcher/configuration checks passed. The old default
  NAND remains intact; `nand-agent-v2` is the separately validated candidate.
- Native snapshot restore confirms the agent reclaims its unsaved session and
  corrects the guest clock. Added AMC, I2S and CS42L58 VMState in separate commits,
  each with a home-screen save/restore check. Active audio playback acceptance
  is still pending; a home-screen restore does not establish that result.
- SDIO now saves sparse firmware memory and partially consumed queued frames,
  in addition to registers, association and timers. Dropping those would lose
  control replies and interrupt masks. Bounded serialization sanitizer checks
  pass; fresh HTTP requests through guest Wi-Fi returned 200 before and after
  restore (`/tmp/it-sdio-snapshot-native.log`). Wi-Fi restore is no longer XFAIL.
- USB reconnection, Bluetooth UART state, H.264 bit-reader state and host video
  decoder cleanup are committed. Native restore passed fresh USB pairing and
  Wi-Fi HTTP requests. Saves with live host GL objects are refused; Light Touch
  reports that reason and binds snapshots to the loaded emulator build and NAND.
- Active audio restore remains unverified: the current graphics engine leaves
  three live GL contexts in the software-compositor test image, and the save
  guard correctly refuses it. Replacing the stale baked engine did not remove
  this limitation (`/tmp/it-audio-snapshot-current-engine.log`). Active video
  restore also needs acceptance; no full host GL serialization is implemented.
  The user's removal of Resume on Launch supersedes the plan's default-resume
  proposal; do not restore that menu feature.

### Regression harness follow-through

- The default IPA is now the repository's Harness.ipa, replacing a private app
  path. The default guest-agent check verifies readiness, ping, shell arithmetic
  and an exact 70 KiB binary round trip. Device-free checks reject operation
  failures and successful responses containing corrupt bytes. The remaining
  Harness graphics/audio integration is now implemented: app install/launch,
  GLES, agent and stereo audio join the default tier. GLES uses the baked
  renderer unless `--stage-gles-shim` is requested, with a Harness fallback when
  GLTest is absent. Native GLES passed. The new audio check detects a real silent
  host output despite nonzero raw I2S samples. This was traced to dock-ID ADC
  channel 3 falsely reporting a Line Out accessory. The open-input default now
  selects Speaker and passes 5.99 seconds at 440/880 Hz, along with boot and
  agent checks (`/tmp/it-regress-open-dock-native.log`). Snapshot audio restore
  remains independently blocked by live GL state.

### Expanded default tier accepted

The combined default run passed boot, full-volume fsck, clean-reboot persistence,
Harness installation and foreground launch, GLES color/hold checks, the guest
agent and 5.99 seconds of 440/880 Hz stereo audio. Both shutdowns were guest
confirmed. Evidence: `/tmp/it-harness-full-default.log`.

New ASan/UBSan tests decode production multitouch frames (empty, one-to-five
contacts and sparse slots), both guest versions' SDIO event headers, and actual
tcp_usb framing under fragmented reads/writes. The USB test replays five saved
real-device AFC file vectors plus synthetic odd-sized transfers and NAK replies;
it validates transport bytes, not a guest AFC server implementation.

### Attitude model

The accelerometer now has one mounted gravity calculation for pitch, roll and
upright/flat poses. Machine properties accept startup and live values; discrete
orientation commands share the same calculation, and raw axis overrides remain
available. The macOS bridge exposes an attitude call. Snapshot v2 preserves the
angles/pose, validates bounds, and restores a resting sensor after a transient
shake; v1 snapshots recover attitude metadata from their orientation.

Sanitizer checks cover cardinal and combined angles, one-g magnitude, invalid
inputs and snapshot metadata. Native paused-QEMU checks cover startup arguments,
live QOM updates, mounting signs and large raw-axis clamping:
`/tmp/it-attitude-qmp-v2.log`. Light Touch inputs and sensor sampling remain next.

### Light Touch motion inputs and sampled sensor

Light Touch now has Upright/Flat poses, two-axis off-panel scroll tilt, Option-arrow
keyboard tilt at three speeds, and Option-Space shake. Focus loss, sleep and key
release return to rest; focused production-method tests cover those transitions,
Live Text and guest-key isolation. Release build passes. Commits: Light Touch
`60b06ef`, macOS bridge export `c9b118aea1`.

The sensor samples on virtual-time boundaries (automatic 100/400 Hz or an explicit
1..400 Hz override), adds bounded one-count noise, and applies a 200 ms three-axis
shake. Native Harness acceptance passed callbacks and guest-confirmed shutdown
(`/tmp/it-accel-guest-native-v4.log`); its requested 100 ms UIKit interval measured
about 100.7 ms. Sanitizers cover rates, saturation, noise, shake and snapshot v3.
QOM axis readback reports requested raw values, while I2C exposes sampled values.

The guest revealed that 7E18 UIKit preserves raw X/Y signs and inverts Z, unlike
an earlier assumption in the plan. The shared model currently retains the plan's
raw-axis convention; physical face-up/steering acceptance remains open. See
`docs/accelerometer-controls.md`. Controller inputs and the attitude indicator
remain unfinished; the optional phone companion has not been built.

### App regression controls use the agent

Default app launch, lock/foreground queries, Harness reset and GLES diagnostics
now use the guest agent when available; legacy fixtures retain the explicit SSH
launcher, and shim staging still requests it. SpringBoard restart uses the same
agent path with bounded read-only readiness retries. Submitted mutations never
fall back to SSH. The launcher now prefers the current native build and honors
`QEMU`; interrupted runs stop their owned child processes.

The full eight-check default tier plus respring passed with `guest_ssh` replaced
by a function that raises on every call: `/tmp/it-agent-control-native-v3.log`.
This includes both guest-confirmed shutdowns, reboot persistence, full-volume
fsck, GLES and 5.99 seconds of 440/880 Hz audio. Focused failure/fallback tests
also pass. The updated, signed app is `build-native14/Light Touch-motion.app`.

### Refreshed default image

`nand-agent-v3` is a separate copy of the agent candidate, refreshed with current
MBX, launch helpers, command/typing agents and all five sound defaults. Baking
now sets the same Core Animation environment as the app's upgrade path. A new
HFS-catalog check validates exact helper bytes, guest ownership/modes, launch
configuration and preferences (`test_baked_components.py`).

The candidate passed the complete default tier plus respring with SSH disabled,
including accelerated graphics, 6.36 seconds of stereo audio, both PMU-confirmed
shutdowns, persistence and full-volume fsck: `/tmp/it-nand-agent-v3-native.log`.
The packaging script prefers v3 when available; an explicit LTM_NAND still wins.
Existing devices retain their selected base and overlay via DeviceStateStorage;
the refreshed base is for new devices or an explicitly requested factory reset.

### Native music-library import foundation

Track D's proposed legacy iTunesDB writer is superseded by measured 7E18
behavior: Music uses SQLite, and its native MusicLibrary import service adds
staged songs while maintaining its own indexes, locations and Purchased list.
`contrib/it-media/itmedia` validates metadata/paths/firmware, runs as mobile,
serializes imports and reconciles an already-imported location read-only.
The identity query avoids private collation indexes created by Music.

`tests/ipod/test_media_guest.py` passes two generated AAC/MP3 imports, duplicate
recovery, malformed-input rejection, Harness MediaPlayer count, Music Songs and
playback screenshots, 12.19 seconds of 440/880 Hz audio, cold persistence and a
second duplicate check. Both shutdowns are guest-confirmed. Evidence:
`/tmp/it-media-guest-native-v4.log`. Music's MIG service must be running before
the third-party MediaPlayer query; an unavailable response is reported explicitly.

Light Touch import/drop/progress integration, photos, artwork, additional
playlists and video-library import remain unfinished. The helper preserves
staged audio on ambiguous failure; retries must reuse the same staged location.

### Light Touch music import

File > Sync Media and audio-file drops now prepare an immutable copy, read
AVFoundation metadata, validate supported codecs/rates and queue AFC uploads
with app installations. Progress uses the existing inspector rows; cancellation
ends at the library commit boundary. The guest helper is included in packaging.

Release build and focused preflight/upload checks pass. The native app-side
test compiles the actual Swift preflight, DeviceServices, IMobileDevice and
DeviceTools music methods; real AFC preserves the exact audio bytes, and the
guest receives quoted/Unicode metadata unchanged apart from canonical Unicode
normalization. Repeated commit of the same staged location leaves one song.
Music's Songs screenshot and PMU-confirmed shutdown pass:
`/tmp/ltm-media-native-v2.log`. The test uses a local adapter to the isolated
guest agent; it does not claim interactive AppKit picker/drop acceptance.

At that checkpoint Photos, raw AAC transcoding, artwork, playlists, content
deduplication across separate jobs and recovery UI remained open. Photos is
implemented and accepted below; the other media extensions remain open.

### Native Saved Photos foundation

`itphoto` uses UIImageWriteToSavedPhotosAlbum after checking baseline JPEG
dimensions/size. Its persistent pending/done receipt distinguishes a confirmed
save from an uncertain outcome and prevents automatic duplicate replay. iOS
creates the DCIM original, album poster and BTH/THM thumbnail files. Confirmed
saves remove only the redundant staged copy.

Native validation passes image colors/dimensions, thumbnail files, duplicate
and uncertain receipts, malformed input, cold persistence and both shutdowns:
`/tmp/it-photo-guest-native-v2.log`. This run matches Light Touch's enabled media
hardware. The album poster and full-size photo render, but the grid thumbnail
is blank. That issue remains open; the test does not claim grid rendering passes.
Host photo preparation and UI integration are in progress, not yet packaged.

### Photos RGB555 thumbnail rendering

The blank grid was an unsupported L555 IOSurface, not missing thumbnail files.
The bridge now imports little-endian RGB555 with opaque alpha and writes FBO
results back as RGB555 while preserving guest row padding. Native CGL sanitizer
checks cover colors, bit-15 independence, writeback and sampled-memory refresh.
An isolated 7E18 Photos run shows the red/green/blue thumbnail and white circle:
`/tmp/it-photo-grid-rgb555.log`. The photo regression now asserts grid colors.

### Light Touch Photos acceptance and refreshed package

Photo preparation (orientation, size bounds, JPEG conversion, white alpha
background and cancellation), real AFC upload, native save and duplicate receipt
checks pass: `/tmp/ltm-photo-native-v2.log`. A separate refreshed default NAND v4
passes component ownership/content validation and the full native Photos test,
including visible grid colors and cold persistence: `/tmp/it-photo-guest-v4.log`.
The Light Touch photo integration is committed as c5e40de. The separate
`build-native14/Light Touch-photos.app` includes current emulator/guest components
and NAND v4; Release build, deep strict signature and macOS 14 library closure
checks pass. Existing device state remains preserved by the normal upgrade path.

### Light Touch battery controls

Device > Battery exposes target capacity with a slider, percentage, Empty/Full
labels and a capacity indicator, plus automatic/forced charging modes. Settings
persist to boot arguments; unsupported older dylibs disable the control. The
runtime bridge validates bounds/readiness and queues both changes on QEMU's
thread. ASan/UBSan bridge and PMU ADC regressions pass. The actual AppKit editor
was rendered at 5/15/80 percent and inspected for colors and unclipped labels.
Guest capacity filtering remains intact; the UI explains the delayed estimate.
Integration into a wider device-status pane remains
open. Earlier native calibration/charging evidence still covers the underlying
PMU properties; no new native guest acceptance is claimed for this UI wiring.

### Refreshed default-image regression

NAND v4 passes boot, full-volume read-only fsck, clean-shutdown persistence,
GLES color rendering, stereo 440/880 Hz audio, binary guest-agent transfers and
SpringBoard restart: `/tmp/ltm-v4-full-regression.log`. That command had an
incorrect IPA path and skipped install/launch; both checks were then run with
the actual built Harness IPA and passed: `/tmp/ltm-v4-app-regression.log`.
The native harness now prefers v4, with v3/v2/legacy fallbacks. Media tests use
v4 too. The battery-inclusive package `build-native14/Light Touch-battery.app`
passes Release build, strict deep signature and macOS 14 dependency checks.

### CLCD interrupt enable and pending status

The 7E18 frame handler disables source bit 0 at register 0x08 when idle. The
model ignored this register and treated an acknowledgement at 0x0c as permission
to keep raising interrupts. It now models enable, pending and W1C independently,
including migration compatibility. See `docs/ipod-clcd-irqs.md` for driver
addresses and the hardware contract. Sanitizer IRQ and LCD compositor tests pass.

The earlier 120-second untethered lock/wake run logged 787 unexpected CLCD
interrupts; the corrected run logs zero. Locked/60-second/95-second screenshots
are black, Home restores the lock screen, the guest agent answers, and shutdown
is guest-confirmed: `/tmp/it-idle-wake-clcd-fixed.log`. This proves display
blanking and wake after idle, not every deep-suspend or automatic-lock path.

All ten native checks pass with the corrected model: boot, full-volume fsck,
persistence, app install/launch, GLES colors, binary guest agent, stereo audio,
SpringBoard restart and serial console: `/tmp/it-clcd-full-regression.log`.


### CLCD package follow-through

The repeatable `test_sleep_guest.py` passes explicit black-panel, Home-wake,
no-reset, agent-recovery, no-unexpected-interrupt and clean-shutdown assertions:
`/tmp/it-sleep-guest-final.log`. Photos import, visible grid colors and cold
persistence also pass after the IRQ change: `/tmp/it-photos-clcd-regression.log`.

### Raw AAC import

Light Touch now accepts raw AAC and streams it through macOS audio codecs into
an AAC-LC M4A before upload, without an external converter. Reads use the decoded
frame count because AVAudioFile reports eofErr on a read beyond an ADTS file's
end. Conversion bounds duration/output size and checks cancellation between
buffers. Existing MP3/M4A/WAV copies remain byte-identical.

Actual Swift preparation passes raw AAC plus the existing audio fixtures. Native
AFC/import/duplicate reconciliation passes and Music plays 6.21 seconds with
440/880 Hz channel peaks: `/tmp/ltm-aac-native-v3.log`. Release build passes.
The one-song playback test uses the first row, without the Shuffle row present
in the earlier two-song test. Music leaves the foreground during volume setup;
no CrashReporter files were present, and relaunch/playback passes. That separate
foreground transition remains unexplained and is not claimed fixed.

### Automatic lock acceptance

`test_sleep_guest.py --automatic` sets one-minute Auto-Lock through Settings in
an isolated guest, verifies a black panel after 95 seconds, wakes with Power,
checks agent recovery without reset or unexpected CLCD interrupts, and confirms
PMU shutdown. `/tmp/it-auto-sleep-guest.log` passes. The default NAND retains
its existing Never setting.

### Automatic battery drain and USB control

The PMU samples fractional percent-per-minute drain against virtual time, with
zero disabling it. Pause and USB charging freeze drain. Native acceptance
`test_battery_guest.py` passes target reduction, guest voltage 3917 → 3906 mV,
pause freeze, USB charging freeze and guest-confirmed shutdown in 114 seconds:
`/tmp/it-battery-drain-native-v5.log`. The guest's filtered capacity initially
moves only 60 → 59; it is deliberately not overwritten by the host target.

7E18 can defer voltage sampling with USB connected but charging forced off.
The native test uses normal unplugged discharge. Light Touch therefore exposes
virtual USB connection alongside level, charging and drain, with transfer guards
and automatic reconnection before restart/Power On. Agent-based sync/shutdown
remain available while USB is disconnected. Core/bridge sanitizer checks and
the Release build pass; the production editor and controller checks cover
fractional input, invalid rates, transfer guard and reconnect behavior.

### Guest audio in screen recordings

Light Touch records the QEMU output mixer directly as stereo AAC alongside H.264,
without microphone capture. Generation-scoped bounded packets share a monotonic
clock with video. Explicit zero PCM preserves idle and VM-pause gaps; timestamps
alone let AAC collapse those gaps. Stop drains queued samples before finalizing.

`test_recording_audio.py` passes sanitizers for packet boundaries, overflow,
clock gaps, stale generations, double cleanup and post-stop draining. Light Touch
`tests/check-recording-audio.py` verifies the production AAC/video writer.
`tests/check-media-native.py --aac --recording` embeds the actual QEMU dylib and
production movie writer, imports AAC through native Music, verifies 440/880 Hz
stereo before/after a two-second silent VM pause, and confirms guest shutdown:
`/tmp/ltm-recording-native-final.log`. Release build passes.

Music's intermittent first Songs-tab exit is reproduced before volume input;
volume buttons are not the cause. No CrashReporter output accompanies it. The
playback acceptance relaunches Music and does not claim this exit is fixed.

### Settings, menus and controller input

Light Touch now has a fixed-size Settings window for rotation, keyboard tilt
speed, catalog URL, controller stick response and A-button coordinates. Resume
on launch remains removed. Device menus expose the existing volume, pause,
snapshot and power-off actions; rotation yields to text editors and saved-state
discard confirms first. Dock actions reuse the same window controller.

Controller input uses Apple's GameController framework on the existing display
tick: left stick maps through a 0.1 deadzone to ±45 degrees, A holds a configured
screen point, Menu sends Home and Options shakes. Native controller snapshots
verify button edges, focus/sleep suppression, disconnection release and response
curves; production touch tests verify release before mouse takeover. Keyboard
motion, Settings and Release checks pass. Physical-controller/game acceptance
is still pending. API reference: https://developer.apple.com/documentation/gamecontroller/gccontroller

### Music's first-launch exit

The guest trace caught `SyncHelper._delayedTerminate` calling
`UIApplication.terminateWithSuccess`, status 0. The helper now completes native
ITSync post-processing synchronously before reporting import success, so Music
need not perform unfinished import work on its first launch. Three fresh-overlay
native Swift/AFC/import/playback tests pass without the former recovery relaunch,
including Songs, volume input, stereo playback and confirmed shutdown:
`/tmp/ltm-music-postprocess-native.log` and `-v2.log`/`-v3.log`.

### Capture and controls package acceptance

The native recording test also passes after the Music lifecycle fix:
`/tmp/ltm-music-recording-postprocess.log`. A 40-point attitude horizon now shows
pitch/roll during tilt and provides an accessible Level action; native rendering
and action checks pass. Help is a bundled, searchable native text window.

### Content-based music import reconciliation

New song imports derive their IDs from prepared content. Existing AFC files are
compared byte-for-byte before reuse; a mismatch is rejected without changing the
library file. New media uploads publish only after close/rename, so interrupted
writes never truncate a canonical library file. Repeated raw AAC conversion now
zeros only generated mvhd/tkhd/mdhd creation/modification timestamps; the audio
payload is unchanged. Bounded container, conversion identity, native repeated
import, mismatch rejection and stereo playback checks pass, as does native
Photos after the shared upload change. Evidence: `/tmp/ltm-media-dedup-native-v2.log`,
`/tmp/ltm-photo-atomic-native.log`, `/tmp/ltm-media-identity-check.log`.

Existing random-ID song imports are not retroactively indexed. Photo content
identity remains pending because its receipts need to distinguish a saved photo
from one subsequently deleted in Photos. Photo retry receipts retain their
previous behavior; new photo uploads gain atomic publication only.

### Recovery and Apps menu follow-through

Light Touch session-tags staging files so a delayed startup sweep cannot remove
new uploads. It also removes only recognized abandoned atomic media temporary
files; canonical media, receipts and unknown files remain. Native Photos/AFC
acceptance seeded old and current uploads, swept them, verified preservation,
then checked the single saved photo and guest-confirmed shutdown
(`/tmp/ltm-photo-cleanup-native.log`). The Apps menu shares the inspector's
existing actions, using selected rows while context menus use clicked rows.
Native menu checks cover selection, batches, cancellation and empty lists.

### Retired-service fast failure

Direct proxy mode returns HTTP 410 for exactly `api.openfeint.com` and
`gdata.youtube.com`, including CONNECT, before contacting an origin. Dated,
disabled and upstream modes bypass the policy. Host sanitizer tests cover
authority parsing, lookalike domains and archive access; TLS regressions pass.
Native NSURLConnection returned prompt HTTP 410 for both hosts after the normal
HTTP fixture passed (`/tmp/it-proxy-retired-native.log`). This is a failure path,
not service revival or a blanket advertising/analytics block.

### Native logs and first typed configuration option

Light Touch's File → Device Logs displays bounded, off-main-thread tails of the
current and rotated serial/kernel and USB logs. It supports Find, selection/copy,
pause, rotation and close/reopen; native window checks and the Release build pass.
App-event persistence and replacing informational alerts remain separate work.

The first configuration migration is `audio-hw=auto|on|off`, listed by QEMU's
machine help. Explicit options override `IT_AUDIO_HW`, including explicit auto;
legacy defaults remain and runtime topology changes are rejected. Sanitizer and
harness checks pass. Native boot/install/launch and 6.18 seconds of 440/880 Hz
audio passed with `audio-hw=on` overriding `IT_AUDIO_HW=0`
(`/tmp/it-audio-config-native.log`). See [configuration](configuration.md).
The unused app-side `IT_IMG3_SIG_ASIS` setting was removed, and boot-parity checks
now understand the current verbose/kernel-console expression and reject obsolete
switches. The rest of the configuration migration is pending.

### App-ledger collection

`regress.py --ledger DIRECTORY` now runs each IPA sequentially on a fresh guest,
retains 5/20-second screenshots and 30-second launch verification, records the
file SHA-256 and bundle ID, and atomically updates Markdown/JSON progress.
Skipped/failed checks and changed input files cannot pass. Rendering, audio,
input and networking stay unreviewed. A native Harness run passed; its initial
menu screenshots were reviewed and retained in [the ledger](app-ledger.md).
Broader game coverage and Legacy Store verdict presentation remain pending.

### Stock-service protocol research

A disposable 7E18 Weather guest accepted local synthetic XML, persisted both
replacement city names, displayed the six-day forecast, and shut down cleanly
(`/tmp/it-weather-protocol-native-v2.log`). The runnable protocol check retains
its screenshot and sanitized requests. [Protocol notes](stock-service-protocol.md)
record the strict six-forecast shape and Stocks' captured request types.
Live provider integration and native city-search acceptance remain pending;
no fabricated weather or quotes are served by the production proxy.

Native city search now displays and saves an opaque replacement Weather ID.
The consolidated `test_stock_services_guest.py --stocks` check verifies all ten
persisted synthetic quotes, including price/change and market-cap conversion.
Its 60-point synthetic chart rendered the expected sawtooth; both guests shut
down cleanly (`/tmp/it-weather-search-native.log`, `/tmp/it-stocks-protocol-native.log`).
This establishes protocol acceptance, not live Weather/Stocks revival.

### Built-in live Weather

Direct HTTP proxy mode now serves the stock Weather gateway from Open-Meteo:
city search, the two default cities, new opaque IDs, current conditions and six
forecast days. Native Foundation handles XML/JSON; requests and responses are
bounded, host TLS is verified, and invalid/unknown data fails without invented
weather. Existing unknown Yahoo IDs need removal/re-addition. The Weather link
and Light Touch Help attribute the noncommercial free API. The moon icon uses a
documented mean-cycle approximation; English search names and unavailable polar
sunrise/sunset remain limitations. Archive/off/upstream behavior is unchanged.

Sanitizer/parser/framing checks and HTTP/TLS regressions pass. A fresh native guest
rendered live Fahrenheit forecasts, searched/added Cupertino, switched to Celsius,
persisted all three six-day forecasts, and shut down cleanly
(`/tmp/it-weather-live-native-final-v2.log`). Stock Weather converts cached
Fahrenheit data for Celsius display. Release app build and native Help checks
pass. Stocks remains at synthetic protocol verification, without a live provider.

### Package and tooling follow-through

`build-native14/Light Touch-latest.app` now points to `Light Touch-45c117d.app`,
with the live Weather helper and Help attribution. Deep signature verification
and macOS 14 dependency-closure checks passed (`/tmp/ltm-weather-package-check.log`).

Screenshot normalization, recording and the regression harness now share one
bounded PPM reader. Truncated headers/comments fail instead of looping forever;
malformed-input checks, existing regression checks and retained native frames pass.
`bootshot.py` uses that reader, fresh overlays and owned-QEMU identity checks. It
stops its disposable guest by default and requires `--keep-running` to retain it.
A native eight-second run captured seven early boot frames and stopped; this is
capture cleanup, not a clean-shutdown/storage acceptance test.

AppSync documentation now describes the four 7E18 sites and entitlement
preservation. The two 5F138-only patch tools and old 500 MB pruning recipe are
marked historical. Networking notes distinguish the old resolver investigation
from current 7E18 behavior; capabilities now link agent/configuration interfaces.

### Native orientation polling (2026-09-06)

Light Touch now uses the guest agent for orientation after media preparation;
only images with no agent retain the streamed SSH helper. Polling preserves
manual rotation, rejects stale boot callbacks, and pauses during sleep/install.
The 7E18 MIG request is firmware-checked and bounds send/receive to 250 ms each,
with a private reply port cleaned up on success and failure. App-name polling
keeps its existing cancellable helper: the native SBS API is also unbounded.

`test_agent_orientation.py` passes ASan/UBSan ABI, timeout, cleanup, firmware and
invalid-result checks. `test_agent_guest.py --orientation` passes native
landscape Harness → Home → stopped SpringBoard (ETIMEDOUT, then ping succeeds)
→ respring → orientation, followed by guest-confirmed shutdown in 87 seconds
(`/tmp/it-agent-orientation-native-final.log`). Light Touch's
`tests/check-agent-transport.py` passes concurrent reply routing, binary exec,
invalid orientation, absent/stale agent and cancellation; Release build passes.

### App-event logs and persistent failure status (2026-09-06)

Light Touch `dacd1af` records existing app events through a serial utility queue,
with UTF-8 byte-bounded entries (32 KB), private files and two 1 MB generations.
File → Device Logs and Export Diagnostics include app.log/app.log.1; export
waits for queued events. Disk-write failure stops further file writes rather
than spinning or growing logs. Unified logging still receives the events.

Preparation, explicit state-save, erase, restore and Power Off failures use a
native titlebar status bar with Show Logs and Dismiss. Notices survive relaunch;
a successful retry clears only its own operation. Storage failures override
other notices and cannot be dismissed while the storage latch is set. Restart,
Erase and Discard confirmations remain decisions. This is not a complete device
status/controls pane or a general alert rewrite.

`check-app-events.py` passes concurrent append, format/literal-percent handling,
UTF-8/combining-character bounds, permissions, rotation, write failures, persisted
notice state, resolution and storage priority. `check-log-view.py` passes native
visibility/layout at 360/640 points, Show Logs/Dismiss behavior, selection/pause
and close/reopen. Screenshot capture was unavailable; these are native layout
and control checks, not a claimed visual screenshot review. Release and Help
checks pass. Full catalog/UI checks pass, after repairing stale AFC/rotation
fixtures and replacing a reproduced timer-order race in the queue test with
explicit readiness gates (`cd6309d`). Evidence: `/tmp/ltm-app-events-check-v3.log`,
`/tmp/ltm-device-notice-ui-final.log`, `/tmp/ltm-event-catalog-checks-v6.log`,
`/tmp/ltm-device-notice-build-v3.log`.

### Shared firmware constants and safe kernel patches (2026-09-06)

Exact verified 5F138/7E18 banners now select a shared firmware table. A legacy
MBX register read previously wrote 2.1.1 BCM4325/USB addresses even when the
kernel was unrecognized; those writes and the legacy clock patch are now
5F138-only. 7E18 keeps the modeled PMU RTC path. AMFI default addresses require
a detected mapped kernel; unknown research builds require all three explicit
address/slide overrides. FMSS's existing 5F138 boot-argument buffer is centralized.

Production detector/patch tests pass ASan/UBSan, including partial/ambiguous
banners, early retry, cache/reset, zero writes to unknown/7E18 through MBX,
legacy 5F138 writes and AMFI override guards. Early boot/NVRAM/audio configuration
checks pass. Native 7E18 with AMFI enabled reaches Settings, survives a legacy
MBX read with all five clock/driver regions unchanged, answers the agent, and
confirms guest shutdown in 87.9 seconds (`/tmp/it-firmware-native.log`). Its
retained clock-function bytes confirm no legacy clock patch was present before
the read. Native 5F138 boot, the firmware-selection property and other behavior
properties remain pending; see `configuration.md`.


### Legacy clock trampoline removal (2026-09-06)

A fresh 5F138 boot exposed an undefined-instruction exception with the link
register inside the MBX-injected clock entry. The injected MRC is a Thumb-2
instruction; ARM1176 supports Thumb-1. Both kernels already have a modeled PMU
RTC, so the trampoline and its unused profile addresses were removed. The
compatibility cp15 clock register remains for old research kernels.

The guarded-patch sanitizer test now asserts no write to the 5F138 clock entry.
Native 5F138 reaches the Home screen with its original RTC path
(`/tmp/it-firmware-5f138-rtc.log`, retained `frame-3.png` in
`/var/folders/tp/360v5_ln3lxg5x66gf0rqc540000gn/T/it-5f138-boot-a5ni3ud5`).
The earlier run stopped in the clock exception (`/tmp/it-firmware-5f138.log`).
The newer run later closed QMP around the untethered idle transition; that
separate behavior is not yet diagnosed or claimed stable.


### Legacy idle is not shutdown (2026-09-06)

A PMU trace establishes that the 5F138 idle exit was generated by the emulator:
register 0x10 changed from 0x7f to 0x3f, triggering the old shutdown heuristic.
Letting the guest continue shows regulator/wake-mask setup, final command
0x6f=0x80 and "System Sleep" / "pmu go hib". Register 0x10 is therefore not a
unique shutdown indicator. Removed that heuristic; the verified native standby
command 0x6f=0x90 remains independent of host arming.

The guest remains alive for 120 seconds through this transition
(`/tmp/it-firmware-5f138-nolatch.log`). Home wake did not resume its deeper
hibernation path, and the separate legacy power-off gesture did not complete;
neither capability is claimed fixed. The production PMU write-handler check
now rejects shutdown for the observed idle sequence. ADC/battery and guarded
firmware patch checks also pass.
Native 7E18 still passes firmware-memory preservation, agent operations, Settings
launch and guest-confirmed shutdown with QEMU exit 0 in 87.9 seconds
(`/tmp/it-pmu-no-latch-7e18.log`).

### Keyboard and USB configuration consumers (2026-09-06)

`osk=on|off` is now a typed, startup-only machine property. Explicit off wins
over the presence-based legacy `IT_OSK` alias, including its old empty/zero
semantics. The shell launcher's keyboard/AppSync flags use the property. Light
Touch uses `osk` and the existing `usb-tcp-addr` for its USB session, removing
two process-global environment mutations. Agent typing remains separate.

The production option sanitizer check and existing audio-option check pass.
Four paused native machines verify QOM registration, defaults, alias/explicit
precedence, and rejection of runtime changes (`/tmp/it-osk-native.log`). The
shell parses and the Light Touch Release build passes. The native 7E18 harness
already uses `usb-tcp-addr`, including the shutdown check above.

Legacy hibernation inspection also confirms why ordinary button IRQs do not
resume 5F138: the CPU remains at `0xc005fc90` (`b .`) with IRQ/FIQ masked after
"pmu go hib". Modeling the hardware wake/resume sequence remains open; no kernel
jump or fabricated resume has been added (`/tmp/it-firmware-5f138-sleep-regs.log`).

### Bluetooth controller configuration (2026-09-06)

The existing HCI controller now takes startup-only `bt` and `bt-latency-us`
properties. Explicit values override `IT_BT` / `IT_BT_LATENCY_US`. Latency is
per controller, with an unsigned 32-bit microsecond bound and safe nanosecond
conversion; malformed, negative and overflowing aliases fail clearly instead
of passing unchecked `strtoll` arithmetic into a timer. A user UART chardev
still overrides the built-in HCI. Bluetooth peers remain deferred.

`test_bt_config.py` passes nine native paused-machine cases for defaults,
aliases, explicit precedence, bounds and runtime immutability. Existing audio,
keyboard and firmware tests pass; CLI builds. One native 7E18 boot completes
BCM firmware Launch RAM with a 2500-us configured delay
(`/tmp/it-bt-config-bringup.log`). This is bring-up evidence, not peer readiness
or a repeated-boot stability claim.

### Battery-powered native shutdown (2026-09-06)

The NOR reboot test exposed a separate PMU omission. Untethered 7E18 prints
"pmu go stdby", sets register 0x0a bit 0 and waits in AppleD1759PMU
(c05fba80-c05fbacc); only the other power-off path was handled. Implemented
that final command and clear its consumed bit on reset. Ordinary 0x0a=0x10 and
legacy idle/hibernation writes do not request shutdown. Production handler and
ADC/reset checks pass. Native NVRAM write, reboot, readback and untethered
shutdown now pass in 55.2 seconds (`/tmp/it-nor-guest-final.log`).

### NOR programming, erase and snapshot state (2026-09-06)

Replaced program/erase no-ops with GPIO-CS-committed page programming, flash
AND semantics, page-buffer wrap, write-enable/status/global protection and
4/32/64 KiB erase. The fixed 1 MiB array and in-flight transaction migrate in
VMState v2, with bounds validation. Original NOR files remain untouched; writes
survive reset in RAM. Cross-process disk overlay and busy timing remain open.

ASan/UBSan command/protection/bounds checks and NVRAM partition checks pass.
`test_nor_guest.py` verifies native 7E18 flush/reboot/readback and confirmed
shutdown. `test_nor_snapshot.py` verifies flash and an unfinished page program
across native migration before GL initialization. Logs:
`/tmp/it-nor-guest-final.log`, `/tmp/it-nor-snapshot-final.log`.

### Persistent private NOR (2026-09-06)

Added startup-only `nor-rw` for an existing private 1 MiB raw copy. QEMU's block
backend owns write locking and durable writes. Base aliases, malformed/missing
copies and concurrent writers fail at startup. Host write failure sets flash
EPE, stops the machine and reaches Light Touch's persistent storage warning.
Snapshot flash is synchronized after incoming backend activation, on first SPI
activity. Native migration/fresh-process checks and injected I/O failure checks
pass; native iOS NVRAM survives both reboot and a new process, with confirmed
shutdown from both guests (`/tmp/it-nor-persistent-guest.log`, 82.5 seconds).

Light Touch automatically creates an owner-writable copy inside its NAND
overlay. Existing copies are preserved, malformed copies fail visibly, and
factory erase/snapshot freshness already cover files in that directory. The
storage check verifies preservation, independent base bytes and failed-copy
cleanup; Release builds pass. No new user setup is required.

### AES workaround audit (2026-09-06)

The proposed G8 shape replacement is not supported by the current trace.
Native 5F138 uses AESCustom (type 0), not AESUID (type 2), for the three
address-suppressed 128-byte operations. It also performs a fourth in-place
128-byte custom operation at 0x0ff290ac which is currently decrypted normally.
Replacing the addresses with the proposed broad shape would therefore either
miss the workaround entirely or suppress an additional operation. Left the
behavior unchanged pending input/key or signature-level classification.
Evidence: `/tmp/it-aes-legacy-trace.log` and its reported qemu.log; the legacy
boot still reaches Home with the new NOR implementation.

### Content-based photo imports (2026-09-06)

Prepared baseline JPEG bytes now use the existing MediaIdentity SHA-256 UUID
helper. Re-preparing the same content gets the same guest receipt while host
work directories stay unique. A completed receipt removes a restaged JPEG.
ImageIO preflight verifies identical IDs/bytes, orientation, alpha flattening,
size limits and cancellation. The production Swift/AFC/native Photos test
imports separately prepared copies into one library item and shuts down cleanly
(`/tmp/ltm-photo-identity-native.log`, 58.9 seconds). The guest receipt/thumbnail
check also passes through a cold restart (`/tmp/it-photo-receipt-cleanup.log`,
91.8 seconds). Release builds pass.

Old random-ID imports are not retroactively indexed. Receipts track successful
imports, not later deletion inside guest Photos; bidirectional asset
reconciliation remains open rather than silently replaying uncertain saves.

### Watchdog debug configuration (2026-09-06)

`wdt-noreset=on|off` is now startup-only machine configuration, propagated to
the watchdog device instead of read from a process-global cached environment
flag. Default remains off. Explicit values override the deprecated
presence-based `IT_WDT_NORESET` alias (including empty/zero legacy values).
Production setter sanitizer checks and the watchdog write-handler check pass.
Four native paused machines verify defaults, alias precedence, runtime
immutability, harmless normal kicks and actual reset/suppression behavior
(`/tmp/it-wdt-native.log`). Timed watchdog expiry remains unmodeled pending
verified clock/field semantics; this change does not invent a timeout.

### Bounded timer dilation (2026-09-06)

`time-dilation=1..1000000` replaces unchecked `strtoll` and interval
multiplication in the timer device. Explicit startup configuration overrides
`IT_TIME_DILATION`; malformed, zero, negative and overflowing aliases fail
clearly. The default interrupt and counter rates are unchanged. The maximum
guest count times the maximum factor stays below INT64_MAX, deadlines saturate
at the clock limit, and restored active timers reject zero intervals, future
bases and overflowing deadlines.

Production arithmetic/state checks pass ASan/UBSan. Ten native QOM cases pass
(`/tmp/it-time-dilation-native.log`), and an armed timer migrates after a brief
boot (`/tmp/it-timer-migration2.log`; before GL initialization). Default native
7E18 boot, guarded firmware-memory preservation, agent operations, Settings and
confirmed shutdown pass in 69.0 seconds (`/tmp/it-timer-default-guest.log`).
Timers 0–3 and the known counter/deadline clock mismatch remain separate work.

## Retired guest host-file services (2026-09-06)

Removed the unused host-file backend: no caller ever registered its file slots,
while guest read/write/size requests could abort QEMU. The three reserved wire
opcodes and packed request layout remain unchanged; they and unknown opcodes now
return Darwin ENOSYS. Each request starts with a cleared error field so a failed
call cannot contaminate a later successful response.

`python3 tests/ipod/test_guest_service_errors.py` builds an ARMv6 probe and boots a
disposable 7E18 device. All three retired calls with invalid arguments and an
unknown opcode returned ENOSYS; a GLES ping after each returned its magic value
with zero error, and the guest then shut down cleanly. Native build passed.

Clipboard service copies now use the agent's existing checked guest-memory
helper. Failed reads/writes return EFAULT; invalid or failed outgoing chunks
clear staging, and committing without staging returns EINVAL without replacing
the previous clipboard. The native service probe covers unmapped and wrapping
addresses, failed continuation/commit, preserved clipboard and subsequent valid
transfer, followed by confirmed guest shutdown. No wire ABI change.

## H.264 machine option (2026-09-06)

`h264-decode=on|off` now selects the decoder device versus the legacy RAM-backed
register window before machine initialization. Default remains off; explicit
configuration overrides the deprecated presence-based `IT_H264_DECODE` alias.
Light Touch now supplies `h264-decode=on` instead of setting that environment
variable. Native configuration checks verify actual decoder creation, default,
alias, explicit override and rejection of runtime mutation. The existing timer
configuration matrix and watchdog check still pass; Light Touch Release builds.

## Inspector status and keyboard control (2026-09-06)

Light Touch now shows device state, explicitly labelled battery target, proxy
status and guest-agent connectivity above the app list, with an input checkbox
and matching Device-menu command. Agent status reuses the existing health poll
at one-second intervals. The keyboard gate rejects presses while disabled,
sleeping or stopped, while permitting releases. Control-modified shortcuts,
including VoiceOver chords, remain with macOS instead of triggering guest input
or tilt. Battery configuration reuses its editor in a transient native popover;
apply failures stay inline and are logged.

Native checks cover 240/400-point status layout, accessibility text, enabled and
checked states, keyboard preference/power gates, modifier routing, and battery
popover apply/failure/reopen behavior. Release build passes. This is not a full
accessibility audit; keyboard pointer navigation and the AFC detail browser
remain open, as do the other active tracks above.

Keyboard screen navigation is now implemented: disable typing and focus the
screen from the View menu, use arrows to move its visible pointer, Space to hold
a touch, or Shift-arrow to drag. Releases survive modifier/mode changes; focus,
sleep and input-owner changes release the gesture. The pointer follows the
existing display transform and does not alter the framebuffer or recordings.
Production-handler checks cover bounds, repeat handling, multi-key drags,
modifier isolation, disabled states and controller/keyboard handoff; Release
build passes. Full VoiceOver inspection and native game acceptance remain open.

## PKE arithmetic and signature recovery (2026-09-06)

Replaced the hex-string round trip with OpenSSL's fixed-width binary conversion.
Short/zero modular-exponentiation results now retain their correct integer
value in little-endian SEG1, and every BIGNUM/context is freed on success and
failure. Invalid segment sizes and arithmetic failures leave operands intact.
The always-false signed-byte padding comparison is replaced with validation of
the complete SHA1 DigestInfo/padding. Explicit forge mode preserves valid
recovered blocks and only substitutes malformed ones. The unresolved fifth-START
sequencing workaround remains unchanged.

`tests/ipod/test_pke.py` exercises production handlers with ASan/UBSan, generated
1024/2048-bit RSA signatures, short/zero results, malformed sizes, valid block
preservation and explicit forging. Native 5F138 reached Home after fifteen RSA
runs; native 7E18 performed nine RSA runs and passed firmware/agent checks and
confirmed shutdown. CLI build passes.

## Operand-driven PKE (2026-09-06)

The next trace resolved the fifth-START workaround. PKE now performs the
register-selected Montgomery products used by the actual boot verifier, with
readable key length, full 2 KB operand SRAM, 64/128/256-byte segments, sign bits,
A×1 mode and a separately preloaded modulus. There is no command counter or
hard-coded public exponent. See [PKE protocol](ipod-pke.md).

Production ASan/UBSan checks pass for RSA and arbitrary exponents, malformed
commands, memory bounds and compatibility mode. Native 5F138 reaches Home
(`/tmp/it-pke-mont-legacy.log`); 7E18 passes firmware/agent checks and confirmed
shutdown (`/tmp/it-pke-mont-7e18.log`). Native migration preserves selectors,
signs, high SRAM and preloaded modulus (`/tmp/it-pke-mont-snapshot.log`). PKE
migration is version 2; old states lack required fields and are rejected.

## AFC Files detail pane (2026-09-06)

Light Touch's View > Files opens a native column browser inside the device pane;
Files is also a customizable toolbar item. Imports reuse the existing chunked,
byte-progress AFC upload. Different existing files are preserved. Exports use a
private adjacent temporary file and atomic publication; failure/cancellation
preserves the destination. Directory paths, file kinds and lengths are checked.
Zero-byte files are supported. The shared uploader now checks cancellation at
the publication boundary. Device changes invalidate pending directory and picker
results; browser events cannot pass through to the guest screen.

Native Swift/AFC checks pass binary and empty transfers, repeated imports,
mismatched-file preservation, cancelled-export preservation and temporary-file
cleanup, followed by guest-confirmed shutdown (`/tmp/ltm-files-native.log`).
Native AppKit checks pass column navigation, stale replies and 200/320/700-point
layout; the 320-point pane was visually inspected. Release build, deep strict
signature and macOS 14 dependency closure pass. Packaged as
`build-native14/Light Touch-b79bc98.app`; latest-app symlink updated.

## Scaler startup configuration (2026-09-06)

`scaler-decode=on|off` now selects the actual scaler versus its legacy register
stub before machine initialization. Default stays off; explicit configuration
overrides the deprecated presence-based `IT_SCALER_DECODE` alias. Light Touch
passes the option directly. The regression harness now accepts explicit scaler
and H.264 options, and Photos uses them instead of environment variables.

All 18 native configuration cases pass, including actual device presence and
runtime-mutation rejection. Scaler ASan/UBSan conversion/IRQ checks pass. Native
Photos verifies thumbnail colors, import/retry, cold persistence and both
confirmed shutdowns (`/tmp/it-scaler-option-photo.log`). CLI, dylib and app builds
pass. AMC/MPVD and remaining display/GLES environment switches are still open.

## Retired TCP tunnel (2026-09-06)

The unused FD/socket cp15 handlers are removed along with their host descriptor
table. These allowed blocking host accept/receive calls on the emulation thread
and ignored guest-memory copy failures. The plan identifies them as dead, and
repository/guest-tool caller searches found no active consumer. Reserved opcodes
and packed argument layouts remain unchanged; requests return Darwin ENOSYS.
Normal networking continues through the emulated SDIO interface and slirp.

The expanded native service probe verifies all 12 retired opcodes and an unknown
opcode, clears stale errors with successful ping calls, checks clipboard memory
failure recovery, and fetches HTTP from a local fixture using native guest BSD
sockets through SDIO/slirp. Confirmed shutdown passes in 28.5 seconds
(`/tmp/it-retired-sockets-native.log`). CLI build passes.

## MPVD startup configuration (2026-09-06)

`mpvd-decode=on|off` now configures the device at startup (default off), with an
explicit value overriding the deprecated presence-based `IT_MPVD_DECODE` alias.
Register writes and interrupt restoration use the configured device flag rather
than reading the host environment. Light Touch passes the option directly.

All 22 native configuration cases pass. Native migration checks exercise both
modes, decode-failure completion, IRQ assertion/acknowledgement and reset
(`/tmp/it-mpvd-snapshot.log`). Boot and a 70 KiB guest-agent round trip pass with
all three explicit video options enabled (`/tmp/it-mpvd-option-guest.log`). CLI,
dylib and app builds pass. This changes mode selection, not codec capability;
the documented MPEG-4 I/P restrictions remain.

## AMC startup modes (2026-09-06)

`amc-mode=registers|handshake|decode` replaces reset-time environment reads.
Explicit selection overrides all three legacy aliases; decode implies the
handshake. AMC hardware remains controlled by the existing audio-hw option.
Builds without libavcodec reject requested decoding rather than silently
falling back. Incoming AMC state is checked against the configured mode.
Light Touch requests decode directly.

All 30 native configuration cases pass. AMC codec ASan/UBSan checks cover
AAC-LC/HE-AAC/MP3/ALAC, PCM/backpressure, DMA bounds and restart. Nine native
migration cases cover saved registers, reset behavior and incompatible-mode
rejection. Native music acceptance imports two tracks, reconciles duplicates,
checks MediaPlayer's count, records 12.19 seconds of 440/880 Hz stereo, and
verifies cold persistence and both confirmed shutdowns
(`/tmp/it-amc-mode-media.log`). CLI, dylib and app builds pass.

Two test issues were corrected during acceptance: migration inspection must
wait for incoming load completion, and ITSync may emit diagnostics before the
final import acknowledgement. Database-count and byte/audio assertions remain.

## LCD composition startup option (2026-09-06)

`lcd-planes=on|off` replaces the process-wide cached environment switch with a
per-device startup property. Explicit values override the deprecated
`IT_LCD_PLANES` alias. Light Touch now passes all media decoder/compositor
choices in its machine string. Photos acceptance uses those same explicit
options without media environment flags.

All 34 native configuration cases pass. LCD compositor and IRQ sanitizer checks
pass. Native Photos verifies thumbnail colors, native save, retry handling, cold
persistence and both confirmed shutdowns (`/tmp/it-lcd-planes-mode-photo.log`).
CLI, dylib and Light Touch Release builds pass.

The signed package `build-native14/Light Touch-36fccf4ca5.app` includes these
changes and passes macOS 14 deployment checks. `Light Touch-latest.app` points
to it. The Help Book is deferred at the user's request; remaining active plan
work retains its existing scope.

## PKE compatibility startup policy (2026-09-06)

`forge-sigcheck=on|off` is explicit startup-only machine configuration (default
off). It overrides the deprecated presence-based `IT_FORGE_SIGCHECK` alias and
configures the actual PKE device. Arithmetic no longer reads process environment
on every final conversion; reset retains the configured policy. Valid recovered
signatures remain intact, and only the existing malformed-signature compatibility
path can substitute a block. This does not change PKE timing or IRQ behavior.

Production arithmetic/policy ASan/UBSan checks pass, including disabled policy
with the legacy environment present. All 38 native paused-machine configuration
cases pass (`/tmp/it-forge-config.log`), checking actual device policy and runtime
mutation rejection. CLI build passes. No new full guest boot is claimed here.

Timer diagnostics now identify the correct A-D register windows (0x00, 0x20,
0x40, 0x60). The prior range missed timer A and mislabeled the 64-bit counter
control at 0x88 as timer D. The local Samsung timer register reference confirms
the layout; production-handler checks verify one warning per unmodeled timer
and no false warning for counter control. No timer behavior is invented.

## Boot image staging failures (2026-09-06)

Boot ROM, direct LLB and direct iBoot now share checked image staging. Missing,
empty or oversized images produce a clear failure instead of continuing into
uninitialized code or overwriting the next memory window. Guest memory write
errors are checked too. Limits follow the mapped boot ROM/iBoot regions and
the start of SRAM1 after LLB.

`tests/ipod/test_boot_image.py` passes ASan/UBSan file/bounds/write-failure checks;
`--native` passes six actual invalid-image startup cases. Native 7E18 firmware
and agent acceptance, Settings launch and confirmed shutdown pass in 87.8 seconds
(`/tmp/it-boot-image-guest.log`). CLI build passes.

## Snapshot continuation fixes (2026-09-06)

H.264 migration v2 retains unfinished software picture slices, reference mappings,
and bit positions within bounded storage. Native decoder objects are discarded;
the existing reference/slice replay path reconstructs them on continuation.
Version 1 remains readable with no retained partial picture. Invalid or truncated
slice payloads fail without replacing existing slice storage.

The expanded `test_h264_slices.py` passes sanitizer checks across ordinary and
restored partial pictures, mixed slices, reference changes, PCM alignments and
malformed migration payloads. `test_h264_snapshot.py` saves after the first of two
P slices in paused QEMU, restores in a new process and checks exact completed
luma/chroma DMA bytes. CLI build and bit-reader checks pass.

I2S snapshot validation now accepts a partially written producer frame while
requiring the consumer to remain stereo-aligned. The existing audio-output check
restores a half-frame, appends the missing channel, and verifies exact playback;
invalid ring bounds/accounting are still rejected.

These close specific decode/audio continuation defects. Full app snapshots with
live host GL contexts remain deliberately blocked; these tests do not establish
unrestricted game snapshots or end-to-end active app playback restoration.

## Direct boot startup configuration (2026-09-06)

`direct-iboot=PATH` and optional `direct-llb=PATH` now configure boot-chain
substitution before machine initialization. Explicit empty paths disable the
corresponding legacy aliases; overlong paths and LLB without iBoot are rejected.
Reset, audio auto-selection, FMSS boot-argument preservation, SYSIC epoch handling,
DSI replies and UART interrupt compatibility all use the configured device policy.
A host environment variable no longer changes unrelated Exynos UART devices.
The remaining optional `IT_DIRECT_EPOCH` debug override is not migrated here.

All 43 native startup cases pass, including actual UART policy, explicit disable,
legacy precedence and runtime mutation rejection. Audio and DSI sanitizer checks
pass. The native 7E18 firmware/agent test now supplies the explicit path; it passes
with a deliberately missing conflicting legacy environment path, then confirms
shutdown in 77.7 seconds (`/tmp/it-direct-config-guest.log`). CLI build passes.
The regression option appender escapes commas in paths using QEMU's doubled-comma
syntax. Remaining CLI launcher scripts outside this repository retain aliases.

## Narrowed AES boot compatibility (2026-09-06)

The retained 5F138 AES trace identifies the three skipped writes as custom-key,
in-place, 128-byte operations. They are not UID-key operations as the original
plan proposed. A fourth operation of the same shape at 0x0ff290ac must decrypt,
so replacing the address list with a broad shape rule would be incorrect.

The existing three-address exception now additionally requires that measured
shape. Unrelated AES transfers targeting those addresses publish their output
instead of silently dropping it. `tests/ipod/test_aes.py` runs the production
handler with ASan/UBSan and checks actual CBC plaintext for other sizes, sources,
UID operations and the fourth custom-key operation. All pass. The address-based
compatibility itself remains unresolved; fresh native acceptance is pending.
Reference trace: `/tmp/it-aes-legacy-trace.log`, naming its retained output folder.

## SHA-to-PKE snapshot ownership (2026-09-06)

The SHA helper's last published chaining digest and validity flag now belong to
the SHA device instead of process-global storage. PKE reads its own machine's
wired SHA device, so another device or discarded future run cannot supply the
compatibility digest. Guest SHA reset and machine reset invalidate it. SHA
VMState version 2 includes both fields; version 1 lacks sufficient information
and is rejected instead of restoring a missing/stale digest.

`test_sha1_snapshot.py` passes ASan/UBSan ownership, reset and transfer checks
using the production-declared VMState fields, including rewind, fresh destination
and invalid-state restores. PKE production arithmetic/compatibility checks pass
with the updated API. Full native migration acceptance remains pending.

### Snapshot mode compatibility

MPVD migration v2 and LCD migration v4 record the source decoder/compositor
startup mode and reject restore into the opposite mode. Older versions remain
readable; they predate this mode check. `test_snapshot_modes.py` verifies both
on/off configurations save and restore successfully to matching destinations,
and all four mismatched destinations fail. Combined native H.264 partial-picture
and I2S half-frame restoration also passes with the current full build.

### Native SHA/PKE and AES acceptance

The extended `test_pke_snapshot.py` now verifies the actual SHA-to-PKE path
across separate QEMU processes: snapshot digest A, publish/use future digest B
in the source, restore and recover A in the destination, then reset SHA and
verify A is no longer available. Original PKE SRAM/modulus/sign checks remain.
All pass (`/tmp/it-sha-pke-snapshot-native.log`).

Fresh 5F138 AES tracing reached the Home screen, visually checked in the captured
frame. All three preserved custom/in-place/128-byte operations and the fourth
non-preserved operation retain the measured shapes (`/tmp/it-aes-shape-native.log`).
The isolated trace fixture then cleans up its QEMU process; this run does not
claim a guest-confirmed shutdown. The address-specific AES workaround remains.

## Decoder replay and boot arguments (2026-09-06)

AMC migration v2 retains compressed packet history and received-frame counts,
then rebuilds libavcodec without publishing duplicate PCM. Queued PCM, partially
consumed codec output, buffer ownership and DMA completion survive restoration.
The replay budget is 64 MiB, 65,536 packets or 262,144 decoded frames per stream;
exceeding it leaves playback running but rejects saving until stream reset.

MPVD migration v3 retains the latest I-picture and following P-pictures, rebuilding
native references without guest DMA writes. Its limit is 4,096 pictures or 16 MiB;
overflow or uncertain decoder state rejects saving until a successful I-picture
or reset. Older snapshots cannot reconstruct references they never stored.
Native codec sanitizer checks compare restored and uninterrupted decoded pixels.

The combined CLI build passes fresh-process AMC continuation, H.264 partial-picture
and I2S half-frame restoration, MPVD register/IRQ/reset restoration, and all four
MPVD/LCD mode-mismatch rejection cases. The live-GL snapshot blocker remains.

Explicit empty boot arguments now disable the environment alias, overlong command
lines fail rather than truncate, and runtime mutation is rejected. Reset reuses
the patch timer instead of allocating another timer on every reset. The focused
sanitizer check and all 48 native configuration cases pass.

## GLES texture/light/material parameter APIs (2026-09-06)

Implemented GetLightfv, GetMaterialfv, GetTexEnvfv/iv, GetTexParameterfv/iv,
TexParameterf/fv/iv and TexEnviv through the host renderer and guest shim. Their
ten slot offsets were checked directly against 7E18 shared-cache OpenGLES
trampolines as well as the committed slot map. Shared vector cardinality now
rejects unknown light/material pnames; texture queries validate ES1.1 targets
and pnames before guest writes. Transfers use exactly 1, 3 or 4 words and reject
32-bit address wrap. Integer texture-environment colors use native GL integer
conversion rather than a float reinterpretation.

`tests/ipod/test_gles_params.py` passes against actual CGL with ASan/UBSan:
state round trips, scalar/vector canaries, invalid enums without memory writes,
unreadable/undersized inputs and wrapping output pointers. Parameter rules follow
the [Khronos ES1.1 reference](https://registry.khronos.org/OpenGL-Refpages/es1.1/xhtml/toc.html).
Combined native CLI build and the armv6 guest build pass using the relocated
legacy SDK. This establishes host dispatch and verified ABI wiring, not a new
full-guest gameplay acceptance claim.

### GLES/PVRTC coverage and native migration acceptance

The focused renderer work adds 45 entry points across framebuffer predicates,
mipmap generation, write masks, scalar/fixed drawing state, boolean/pointer and
matrix queries, and ten light/material/texture parameter operations. The guest
7E18 dispatch ABI was checked against its retained trampolines. Native CGL
sanitizer checks cover exact query/vector sizes, invalid enums and guest pointer
bounds; unsupported entry points still require further coverage.

PVRTC decoding now covers all 2/4-bpp interpolation modes, fixed-point color
precision, mixed neighboring modes, RGB alpha policy and tiny mipmaps against
20 PowerVR reference vectors. Upload checks cover exact padded or supported
compact sizes, invalid parameters, full-image replacement and texture ownership.
These checks pass under ASan/UBSan and native CGL; they do not establish correctness
for every game or unsupported compressed subregion updates.

`test_mpvd_snapshot.py` now also decodes two actual pictures, migrates to a fresh
QEMU process and verifies the remaining six P-picture DMA outputs equal the
uninterrupted source. Timer migration v2 rejects a different startup time-dilation
value; native checks verify both mismatch directions and same-mode interval
reprogramming after restore. Version 1 predates that compatibility field.

### Focused integration package

The combined guest acceptance passes boot, GLES fixture rendering (magenta/cyan
0.498 each), agent ping/shell/70 KiB transfer, and 6.18 seconds of stereo
440/880 Hz audio with zero failures (`/tmp/it-focused-final-acceptance`). The
expanded startup matrix passes all 72 cases. Light Touch commit `c011bf5` uses
explicit boot-argument/scheduling properties, retaining the app's existing values.

Release Xcode build and self-contained package pass. The signed, deployment-checked
bundle is `build-native14/Light Touch-60ff9e5197.app`; `Light Touch-latest.app`
points to it. Bundled GLES/agent helper bytes match the built sources and the
packed NAND checksum verifies. Existing canceled media source edits remain
uncommitted and were not included in rebuilt guest helpers.

### Settings/Contacts launch regression (2026-09-06)

A disposable copy of the affected Light Touch overlay reproduced the missing
launch zoom. BTServer logged malformed H4 packets and restarted; SpringBoard
logged repeated BTSessionAttachWithRunLoop timeouts. Firmware download had
succeeded, so the bring-up check now also requires subsequent BCM controller
setup. Its firmware-only negative case fails.

PL080 packet-end handling incorrectly zeroed the residual transfer count even
when only seven of 2048 requested bytes had transferred. The 7E18 driver computes
completed bytes by subtracting CTRL & 0xfff from descriptor length: query path
c0750730–c07507b8 and completion path c07519f0–c0751a04. Preserving residue avoids
reporting the unwritten buffer tail as received data. The production-function
sanitizer test fails on the previous implementation and passes on the correction.
The register semantics also match ARM DDI 0196G, section 3.3.11:
https://documentation-service.arm.com/static/5e8e3c6488295d1e18d3a8c3

Related UART receive-mode and FIFO-reset corrections synchronize DMA requests and
cancel stale packet timeouts; bytes arriving with reception disabled wait until
reception resumes. Those fixes alone did not eliminate the observed failure.
Their sanitizer checks independently fail on the old implementations. A native
CGL check also covers internal stencil clearing with guest writes disabled.

With the DMA correction, captured Settings/Contacts launches include intermediate
zoom frames; BluetoothManager getters complete in 0–1 ms on the affected overlay
copy. Two additional quiet cold boots pass explicit non-null/latency checks,
for three successful quiet boots after the DMA correction. Tests use device
copies, not the user's active overlay. Temporary UART
recorders are removed; traced successes were not treated as sufficient evidence
because diagnostic output changes the timing of this intermittent failure.

Release Xcode build, emulator dylib rebuild, packaging, and strict signature
verification pass. The updated package is `build-native14/Light Touch-dma-recovery.app`;
Xcode's development dylib is updated too. Canceled media edits remain untouched.

### 2026-09-19 — UART interrupt mapping restores app launch animations

Reproduced the returning Settings/Contacts animation regression against the native library used by Light Touch's default Xcode build, with networking enabled and a clone of the affected NAND overlay. Quiet boots exposed BTServer malformed H4 replies and restart loops, with BluetoothManager attachment timing out around one second. Verbose DMA tracing masked the failure.

Disassembling the running 7E18 UART ISR at `0xc059aecc` resolved the incorrect interrupt interpretation: UTRSTAT `0x08` is Rx timeout, `0x10` Rx threshold, `0x20` Tx, `0x40` error, and `0x100` automatic baud measurement (reads UABRCNT and divides the clock). The model had mapped Tx to the timeout bit and Rx to the autobaud bit, and did not honor UCON interrupt enables. Corrected the mapping and UCON[14:11] gating, and recompute IRQ state on UCON writes. Removed UART1's synthetic last-request/terminal-count wiring: the driver's timeout callback handles partial DMA buffers without advancing its 2,048-byte controller-counted LLI.

Three independent quiet boots of cloned device data now attach BluetoothManager in 13.3, 11.7, and 13.2 ms, with one BTServer process per boot and no malformed replies. Native LCD frame captures show intermediate zoom surfaces for both Settings and Contacts on every boot. The installed default development library also passes three consecutive stack probes (13.9, 10.5, 10.9 ms). No NAND patch/reset was used. `build-native14/qemu-build/libqemu-arm.dylib` is rebuilt and signed; source changes remain uncommitted.

`test_uart_rx_transitions.py` now exercises the production IRQ code, UCON enables and acknowledgments instead of stubbing update_irq, and rejects the old model. Added `bluetooth-stack-probe.c` and `check-bluetooth-stack.py` for quiet guest-side readiness checks. The firmware-only bring-up check explicitly documents that verbose tracing cannot prove client readiness or restored animation.
