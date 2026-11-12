# Diagnostic bring-up (not protected handoff)

Scope: operator-configured per-app device_free_diagnostic, default false. Internal App flag is skipped
in public serialization. Contradictory compositor/audio/non-process configuration fails before runner
construction. This controls Wolf-managed devices, not arbitrary operator commands or a host sandbox.

Production seam: sessions/diagnostic_startup.hpp owns the actual startup decision and future continuation
used by moonlight.cpp; injected operations stand for device factories/audio/producer/socket readiness,
not a copied lifecycle model. No locks are held across operations. Async continuation retains its
operations and captured session until completion. Compositor ready/error behavior stays separate from
no-compositor runner readiness. Existing runner and producer lifetimes are not redesigned.

Input policy is checked in the common bounded dispatcher, all direct side-effect helpers and lazy
factories. Exact-scope own Plug and lobby Join/Plug guards prevent diagnostic device attachment or alias.
Normal controller slots0..15, scope validation, MWOW6 decrypt/replay and F6/F7 guards are preserved.

Audio startup extraction keeps the existing actual Docker fallback behind a single explicit FALSE gate
before cleanup or Docker operations. Null/disconnected audio cannot dispatch router/handlers/ingress.
Default fallback remains enabled. WOLF_ENABLE_MDNS=FALSE skips the entire worker dispatch; default true
preserves behavior. Manager reported existing upstream mdns alignment UB: disabled discovery is a narrow
mitigation, NOT a fix/suppression of upstream UB. No dependency or sanitizer changes.

No deployment config/key/cert/UUID/state is changed here. Manager later reuses the SAME pairing identity,
adds only one fixed sleep600 ball/ticks app, and sets WOLF_ALLOW_PULSE_CONTAINER=FALSE,
WOLF_ENABLE_MDNS=FALSE with explicit existing Pulse. Live video, physical isolation/gamepad handoff and
WoW readiness are NOT proved by software tests. Independent review and Manager launch remain gates.

The initial extraction preserved the empty Wayland continuation solely for a committed regression:
b6c4e76d5570b2c821fc2dc610c43178030def3f produced 9 expected assertion failures (2 cases/39 assertions).
a822551d586559ebe1ca15c961f444efe5643658 removed that continuation for no-compositor apps;
identical complete fixture bytes passed 39/39 readiness assertions, then 7 cases/256 assertions.
The first compile failure was fixture namespace/aggregate/eventbus-lambda typing, not semantic RED.
Final exact-revision build/suite evidence is kept separately in the diagnostic report and hashed manifest.

Coverage limits: the startup continuation, actual config parser, actual V2 decrypt/common dispatcher,
14 direct input helpers, actual lobby Join/Plug handlers and actual audio/mDNS dispatch are exercised.
Device/Pulse/compositor/listener providers are injected; Wolf main and actual sockets are not executed.
The Unix HTTP wrapper is source-mapped to the tested common dispatcher, not replayed over a socket.
No-sidecar tests prove the production fallback callable is never invoked; source inspection places all
remove/create/start operations inside that callable. No real Docker or filesystem cleanup is tested.
Existing privileged custom-session/CreateLobby APIs remain outside the per-app policy. Isolate their
Unix socket and do not use them in the diagnostic trial. Existing async subsystem lifetimes, concurrent
RTSP writes and live transport interoperability are not closed by this change.

Manager-owned deployment delta (after review, never regenerate pairing identity): keep existing v7
config/profile, UUID, paired clients and key/cert paths; add one process app with diagnostic=true,
compositor=false, audio_server=false, command `sleep 600`, video `videotestsrc pattern=ball is-live=true`
and audio `audiotestsrc wave=ticks is-live=true`. Use existing x264 H264 settings, 640x360 at30fps,
WOLF_USE_ZERO_COPY=FALSE, WOLF_ALLOW_PULSE_CONTAINER=FALSE, WOLF_ENABLE_MDNS=FALSE,
PULSE_SERVER=unix:/run/user/1000/pulse/native and the isolated runtime/state. Exact config field name is
`device_free_diagnostic`; pipeline source overrides remain under `video.source`/`audio.source`.
Manual IP connection uses192.168.88.54. This trial tests video/audio only, not game/device readiness.

## Actual live launch failure correction (after b136 review)

Manager's same-binary sanitized repro identified stack-use-after-scope in
rest/endpoints.hpp:create_run_session: split returned views into the temporary string from
get_header(mode).value_or(...); that owner died at the preceding semicolon. The subsequent
stoi(view.data()) also assumed null-terminated individual fields. The full trace identifies
ref.tmp at line396, read at397; the audio dispatch nested call appears only in thread creation.
No remote video/ANNOUNCE/StartRunner occurred in that failed attempt. This is inherited REST code,
not a demonstrated new audio callback-lifetime defect. Manager separately reproduced two ENet
null-base offsetof UBSan diagnostics; the third identical outgoing expression is source-confirmed,
not separately live-reproduced.

The actual REST parser now retains the mode string, checks at most32bytes before copying, exactly
three nonempty fields and complete positive int conversions with bounded from_chars. No returned
views. Default1920x1080x60 and valid640x360x30 are preserved. This is a representation bound,
not a claim that arbitrary INT_MAX dimensions are physically supported. Invalid mode returns no
session before directories/scopes/registry/event effects; launch/resume return existing400 response
before mutation. Parsing other request fields is not broadly redesigned. surroundAudioInfo stoi
and rikey/rikeyid references are consumed within their full expression; create_stream_session
copies the latter into owned strings. Their existing missing/invalid-field exception policy remains.

ENet fullpin44c85e16279553d9c052e572bcbfcd745fb74abf is unchanged. Tracked patch changes only three
null-base sentTime offset expressions to offsetof and adds stddef.h. Header/wire layout unchanged.
CMake PATCH_COMMAND includes patch SHA for ordinary stamp invalidation; driver accepts only exact
original or known-patched protocol.c plus exact unchanged protocol.h, verifies Git HEAD when metadata
exists, and rejects unknown/partial states. Archive fixtures have content-hash provenance, not a fake
Git identity. Source overrides are verify-only after MakeAvailable: pristine/unknown/partial reject,
knownpatched accepted without mutation. No direct fetched source edits, sanitizer suppression or PIC
change. Safe tests exercise actual parser/session creation (owned temporary directory only) and actual
pinned ENet layout; no sockets/devices/runner. Actual multicast/control/network replay still belongs
to Manager after review. D29-1 protected-test scheduling follow-up remains separate/open.

The tracked ENet diff uses zero context to avoid whitespace-only context lines in the patch artifact;
--unidiff-zero is allowed only after the complete pristine-file SHA check, then exact post-hash verification.
No fuzzy or unknown-source application is permitted.

## Actual custom audio GObject lifetime correction (after 6007 live replay)

The next Manager-owned live replay reached AudioSession but failed in aes_iv string assignment:
GObject's C allocation does not construct embedded std::string aes_key/aes_iv or shared rs_ptr.
Instance init now explicitly constructs all three before assignment/property access; finalize destroys
all three once. Repeated dispose exchanges the shard matrix to null before deletion. Shard/RS preparation
uses temporary RAII owners; a caught allocation failure leaves a destructible instance whose actual output
path fails closed. This does not establish general allocation/exception safety across GObject callbacks:
property string allocations, GLib allocation failures and upstream RS internals retain their existing limits.
AES defaults, key/IV use, RTP/FEC and wire behavior are unchanged. No encryption/audio/SAN workaround.

New isolated tests instantiate the ACTUAL custom audio GType, properties, repeated dispose and finalize,
observe real RS owner expiration, then run a bounded real appsrc → statically registered custom audio
payloader → appsink pipeline. Four dummy encoded buffers exercise AES roundtrip plus two FEC outputs;
the invalid-IV production split catch/drop path is exercised separately. This deliberately expands earlier core-only
fixtures: linked app types and the custom static plugin are exercised, registry scanning disabled; no
external plugin, socket, sound device, main or live session. Existing ten diagnostic test bodies preserved.
Software output is not actual audio/video network delivery; Manager replay after review remains required.

Adjacent read-only audit: video GObject instance contains only scalar fields beyond its C base, so the
same nontrivial-member construction defect is not present there. No video source changes. Audio packet
size limits, property concurrency, general callback exception safety and output flow propagation are not
claimed audited/closed by this lifetime change. Manager's orphan sleep600 inheriting listener FDs is a
SEPARATE observed process-runner residual: no process.cpp fix here; diagnostic wrapper cleanup remains
Manager-owned. D29-1, mDNS alignment mitigation and previous live/physical/isolation gates remain open.

The new negative short-key test exposed a separate inherited CBC size-validation gap (one-byte key
produced a packet; three assertions failed, preserved as evidence). Manager approved a narrow additional
boundary in the owned actual generate_output: reject encrypted keys of length other than16 BEFORE split/
OpenSSL; clear output, retire the transferred input once, set sticky encrypt_failed. Failed instances cannot
implicitly recover by later property assignment. Fresh encryption=false behavior remains unchanged.
The negative GObject virtual-method test now exercises this actual boundary; a separate valid-key/invalid-IV
case exercises the existing catch/drop helper. This is not a fix to all crypto helper callers. Initial fixture
compile namespace ambiguity and static-registration C linkage errors are preserved as build failures, not RED.

## Supervised legacy PadOnly trial — source checkpoint (not enabled or tested live)

`legacy_pad_only_trial` is a separate app opt-in, absent=false; `legacy_pad_slot` is absent=0,
validated 0..15. Config parsing rejects diagnostic+trial, compositor, audio-server or non-process
runner combinations before constructing app effects. Runtime flags are Skip fields, not client wire
settings. Config save projection preserves both opt-ins and the selected slot. Existing video/audio
override save limitations are not fixed: no operator config/API mutation during the trial.

The actual startup helper suppresses Mouse/Keyboard/Wayland/VSink construction and socket wait for
this mode, then invokes the existing runner once. Ordinary no-compositor and compositor readiness
paths remain; device_free_diagnostic still rejects ALL input. App policy must be chosen before a
fresh stream/host, never changed on a session that already has devices.

The actual common dispatcher permits only selected-slot ARRIVAL/MULTI with a non-null ControlPeer
and sender, valid captured scopes and existing packet bounds. Unix input supplies a null peer and
is refused. **ControlPeer is not an authentication capability by itself**: the unchanged actual
ControlLoop::State::dispatch checks current connection/run, key possession, V2 replay/nonce and
current generation again before this call. Direct internal typed entry points require that same
trusted serialized caller; no invented per-request approved bool or probing send with an empty
payload. The appended actual-loop fixture replaces only host/service IO and exercises same-ID
connection replacement before input dispatch, reused raw peer allocation, stale sender refusal and
fresh authentication. It uses inactive MULTI even on a regression, never a device-opening packet.

The common device type selector forces Xbox despite PS/motion/client overrides. Only the selected
slot is retained in session.joypads; a nonempty incompatible inventory is refused. Active MULTI can
create it before ARRIVAL; inactive MULTI with no pad is a no-op. Repeated ARRIVAL and mask cycles
reuse it. Inactive masks call existing buttons/sticks/triggers setters with zeroes, keep the pad,
and do not perform kernel delivery verification. Trial frames have no UI combo handling. Non-pad
common/typed/lazy paths refuse mouse/keyboard/text/pen/touch/motion/battery and other pad extensions;
no rumble/LED/adaptive callbacks or motion requests are registered/sent for the trial pad. Plug/Unplug
relay and Join refuse isolated sources and available isolated targets (matching running RunScope or
retained lobby members); Lobby has no standalone App policy. Existing exact membership guards remain.
The host desktop Wine path needs no Docker hotplug/Wayland alias; no trial udev-event lookup/relay.

`control::detail::TrialPadIO` is a synchronous, per-call internal factory/lookup/setter boundary,
never stored or configured publicly. Production supplies the actual session inventory and common
Xbox factory; tests replace only IO after real dispatcher validation. `apply_trial_frame` is the same
production setter mapping exercised by recording IO. Normal input does not construct this IO adapter.
The actual type selector is separately exercised for PS/motion/overrides and unchanged normal behavior.
No fake device or stub inputtino replaces production. New regression cases are **prepared, not run**
in the source-only phase; all original 14 diagnostic test bodies remain byte-identical. Native
compile/SAN/tests and independent review require Manager's separate build grant after live host stop.
No runtime PASS, enablement, device creation, Wine mapping or fresh build claim accompanies this commit.

Boundary of “one”: ONE selected pad in ONE supervised trusted iPad stream, not an enforced process-wide
singleton/GameACL or all-routes policy. No parallel launch/client, no handoff, no privileged Unix mutation
in the trial. Protected InputSeat/Handoff/checked backend remain unwired. Legacy Xbox uses uinput,
not uhid, but still starts its existing detached FF listener internally even with feedback callback
unset. Void setters/cache and inactive-mask neutral do not prove kernel acceptance, joined retirement
or neutral on pause/disconnect. Release controls before disconnect; Manager owns supervised cleanup.
No new worker, global singleton or reaper was added. `/dev/uinput` ACL alone does not prove Wine access
to newly created event/js nodes or game hotplug/mapping; those remain separate supervised runtime checks.
No `/dev/uhid` permission is needed/requested. Current game/prefix and deployed binaries remain untouched.

### Native trial verification follow-up

Before build, the lobby target guard was corrected to retain the members snapshot for the entire
iteration. Initial new-fixture compile failed on ambiguous UNKNOWN and generic eventbus lambdas;
concrete packet namespace/callback types fixed that without changing the original14-case prefix.
The first targeted8-case run passed assertions but revealed an unintended real Docker GET /version:
normal config construction still held AppDocker after the trial rejection check. It is NOT counted
as isolated software-only evidence. The new case now checks normal Docker policy without constructing
a daemon client, then constructs an actual normal process app. The old unchanged config case has the
same inherited constructor probe; full-suite execution needs an explicitly approved inaccessible
Docker socket environment guard. Raw first run remains preserved; no container mutation was attempted.
Final native/SAN status is recorded in the accompanying thread-storage report, not inferred from this
historical source checkpoint or from a green assertion count alone.
