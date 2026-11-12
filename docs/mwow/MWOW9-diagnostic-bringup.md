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
