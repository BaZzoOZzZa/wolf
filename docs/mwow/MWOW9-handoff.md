# MWOW-9: scope foundation checkpoint

Status: **review**, foundation only. Base `d57e6b0145fb8733b4ec0634604a453ebc7c2478`.
This is the first atomic step of R1/R6/R9 (and the device-routing part of R5),
not a completed handoff, release candidate, or deployment permission.

## Stable internal interfaces

`wolf::core::sessions` in `sessions/connection_scope.hpp`:

- `RunScope::fresh(std::string)` identifies one admitted run lifetime. Launch
  allocates it; resume retains it. LobbyLifecycle allocates it once per Entry.
- `ConnectionScope::fresh(std::size_t)` identifies one launch/resume connection.
  A copy retains identity. Recreating a scope with the same ID creates a different
  identity, regardless of reused RTP/ENet secrets, crypto object or random output.
- `scope.matches(other)` requires two non-null handles with the same retained
  epoch allocation and ID. Even null/null is false. No implicit wildcard exists.
- `targets(owned, requested, id)` additionally verifies the event's public ID.
- `resource_key()` is an opaque process-local name. The retained allocation cannot
  be reused while an event/worker still holds it. It is not a durable identifier,
  authentication secret or public API field.
- `resolve_producer(consumer, requested, session_id, producer, producer_id)` returns
  only the captured run's resource key, or nullopt. Both real audio/video switch
  callbacks use it. `ProducerBinding::select` serializes the property change and
  retains the selected RunScope after the event disappears (otherwise a listening
  sink name could suffer allocator ABA). This is not full pipeline serialization.
  Interpipe sink names and initial default-source bindings use
  this key, so old switch events cannot discover a replacement by numeric ID.

Connection events carry `rfl::Skip<ConnectionScope> connection_scope`:
IDR, Pause, Resume, Stop, UICombo, VideoSession, AudioSession, JoinLobby, LeaveLobby,
SwitchStreamProducerEvents, and device-routing source events.
VideoSession/AudioSession also carry `rfl::Skip<RunScope> run_scope`.
Join/Leave carry `lobby_scope`; Switch carries `producer_scope`.

`GameStopRequest{session_id, run_scope}` is an explicit run stop request.
`RunEnded{session_id, run_scope}` is a terminal runner/startup fact, not a stop request.
StopLobby retains its public name and PIN but gains a hidden RunScope.
Runner::run now takes `const RunScope&` instead of a session string; all owned
runners and `RunnerArgs` adapters propagate it. No ID-only overload remains.
StartRunner retains the captured StreamSession, including both scopes.

Device plug/unplug events carry destination RunScope and originating ConnectionScope.
The queue map retains `{RunScope, shared_ptr<queue>}`; worker launch copies the
queue ownership while the immutable map snapshot is alive. Docker callbacks and
queue dequeue validate the destination run. Docker lifecycle notifications carry
that same run. This is **not yet** a device-generation/worker completion contract.

## Inventory: runtime emitters and consumers

Paths in this table are under `src/moonlight-server/`.

| Event / ingress | Emitters and capture | Consumers and adaptation |
|---|---|---|
| StreamSession | `state/sessions.hpp` allocates both scopes; REST launch and Unix add publish state before firing | `sessions/moonlight.cpp` rejects missing/noncurrent connection; retains scope in startup callbacks, queue and producers |
| REST resume | `rest/endpoints.hpp` allocates a fresh connection, retains run/display/devices/audio/held modifiers | Compare-and-replace uses old scope; failed replacement returns error. Old StopStream captures the old scope. Completion/drain admission remains next stage |
| VideoSession / AudioSession | `rtsp/commands.hpp` copies both scopes; Unix StreamSessionStart resolves once and overwrites any deserialized scope | `sessions/moonlight.cpp` verifies captured connection before waiter and after ping; `streaming.cpp` rejects null scopes. Waiters are still unbounded/detached |
| PauseStream | Authenticated TERMINATION in `control/control.cpp`; explicit Unix pause resolves once | Video/audio consumer checks exact scope; **no lobby leave, runner stop, or session removal** |
| ResumeStream | First authenticated control packet copies the associated connection scope | No runtime consumer currently exists; notification only |
| IDRRequest | Authenticated control packet captures the peer's StreamSession scope | Video consumer checks exact scope |
| StopStream | REST resume targets prior connection; run adapter targets connections belonging to the captured run | Video/audio consumers check exact scope. No direct runner/producer/session-map/lobby teardown |
| GameStopRequest | HTTPS cancel of the paired session, Unix stop, shutdown snapshots | Run-scoped CMD/Docker and producers; moonlight emits exact connection stops but retains session until RunEnded; lobby cleanup uses matching member run only. Network disconnect/TERMINATION cannot emit it |
| RunEnded | Captured Moonlight runner completion or confirmed pre-runner Wayland-socket failure; Unknown is retained | Producer EOS, scoped consumer stop, removal only of matching run, queue erase only of matching run, matching lobby member cleanup |
| StopLobby | Unix PIN-checked ingress captures the lobby; shutdown captures snapshots; lifecycle completion and deferred stop retain Entry.run_scope | Lobby cleanup validates ticket scope; CMD/Docker/video/audio compare exact scope. DockerContainerCreated retry verifies same run; existing MWOW-7 retry/Unknown/exception semantics retained |
| JoinLobby | Unix ingress captures current connection and target lobby scope once | Exact scope lookup, matching lobby, existing lifecycle admission and PIN checks; captured member snapshot retained |
| LeaveLobby | Unix ingress, UICombo, explicit game end/stop cleanup, lobby cleanup, deferred lifecycle queue | Exact connection + lobby scope; member must belong to same run. Removed sessions can be cleaned from retained member snapshot, without promoting an ID to a new run. Deferred leaves store ConnectionScope, not numeric ID |
| UICombo | Both keyboard and controller combo paths in `control/input_handler.cpp` | Scoped explicit leave; network pause/stop no longer shares this path |
| SwitchStreamProducer | Join/leave captures consumer connection and target producer run | Both streaming consumers use `resolve_producer`; actual destination uses retained run resource key |
| PlugDevice | Initial mouse/keyboard in moonlight; joypad/pen/touch creation in input_handler; lobby join/leave migration and relay | Source relay resolves only exact connection/run; queues preserve destination run; Docker dequeue validates it |
| UnplugDevice | Input inactive-mask path; lobby migration and source relay | Source relay rejects stale connection/run; Docker compares destination run before filesystem/exec |
| DockerContainerCreated/Stopped | `runners/docker.cpp` retains scope alongside immutable container ID | Created stop reassertion validates Entry scope; Stopped currently SSE only |
| StartRunner | Moonlight on-ready continuation and Unix RunnerStart capture full session | Queue lookup must match run before copying ownership to worker; terminal callback keeps captured run. All-route singleton admission remains outside this checkpoint |
| Unix input | Existing endpoint resolves a StreamSession once | `handle_input` now rejects missing/mismatched scope; existing complete packet/field guards remain. Serial InputSeat admission is not implemented |
| ENet association / feedback | Existing MWOW-6 peer state retains session; match rejects missing scopes | `same_control_generation` adds exact ConnectionScope/RunScope to existing crypto/token/key checks; V2/replay/outbox behavior unchanged |
| RTP ping | UDP ingress has only wire correlator / legacy address and port | Cannot mint internal authority. Waiter captures Video/Audio scope, rechecks before streaming; cancellation and duplicate-start reservation remain next stage |

`get_session_by_scope` never creates authority from a stable ID. Run-scoped
administrative cleanup may enumerate the current connection of that **same captured
run** (`get_session_by_run`); it cannot target a new run with the same stable ID.
All delayed lifecycle completion callbacks retain their Entry/run; they do not
resolve a new run by ID. There are no legacy ID-only runner stop adapters.

## Public compatibility

No Moonlight packet, RTP token size, SDP extension or ENet message format changes.
The existing printable RTP-secret / ENet random generation is unchanged; its
mt19937 weakness is not claimed fixed. Wire correlators are not internal epochs.

Existing JSON request fields and reflector projections remain. `rfl::Skip` hides
runtime handles; StreamSession/Lobby explicit reflectors omit the new fields.
SSE remains notification, never a capability that can be replayed to gain authority.
New GameStopRequest/RunEnded notifications are additive. Existing StopStream
notifications still describe consumer stop. Local `/sessions/stop` and paired
HTTPS cancel retain explicit game-stop intent; Pause means connection-only pause.
The intentional behavioral change is that network pause/stop no longer unplugs
or leaves a lobby. API success means accepted, not completed teardown.

An externally delayed administrative ID-only HTTP request still resolves whatever
instance exists **at ingress**. An expected-epoch public API would require a separate
compatibility decision. Internally delayed events retain their original scopes.

Default named interpipesrc elements bind to scoped sink names at pipeline setup.
Custom externally supplied pipelines which hard-code raw `<id>_video/audio` sink
names must migrate to the server-managed named interpipesrc binding; no general
arbitrary-pipeline rewrite or backward alias to a reused sink is provided.

## Verification and limits of this checkpoint

- Existing AST index absent; used targeted ranges/diffs, no rebuild. No agent-tools
  manifest. Canonical source is the assigned ext4 Worktrees checkout, base exact above.
- Cheap standalone production Scope/LobbyLifecycle smoke: C++20, ASan/UBSan, including
  10,000 retained-allocation ABA iterations and deferred/null scope checks.
- Adapted `tests/testLifecycle.cpp`: **13 scenarios passed**, ASan/UBSan. Only scope
  arguments and retained deferred-scope assertions changed; no checks removed.
- New native `wolf-connection-scope-tests` uses Catch2's safe main and actual
  `setup_moonlight_handlers` / `setup_lobbies_handlers`, an owned EventBus and fake
  state. It never emits startup/ping events or initializes GStreamer/devices.
  Covers stale/null RunEnded/GameStop, connection-stop vs game completion, stale
  join/leave, preserved lobby membership on connection loss, scope serialization,
  deferred scope retention, ABA, exact session removal, stale producer switch,
  and retained listener binding after the switch event is destroyed.
  **Authored/syntax-checked; native link/execution awaits Manager's slot.**
- Mechanical legacy adapters: `tests/testLifecycle.cpp` and `tests/testWolfAPI.cpp`
  (real source scope in device relay and control-association fixtures; fake joined
  session receives explicit scopes). `tests/testStream.cpp` does not exist here;
  `tests/testControl.cpp` needs no adapter. Serialization-only IDR SSE fixture is
  intentionally unscoped and does not execute a runtime sink. RTSP fixture sessions
  use the production session factory. No security/field assertions removed.
- Qualified `clang++ -fsyntax-only` uses native MWOW-8 compile commands, changing only
  source root, output/dependency-writing flags, and adding the **same fetched Boost
  1.87 split uuid include**. Baseline native commands omit that dependency; Manager
  reproduced the failure and assigned its CMake correction to MWOW-10. Manager
  subsequently reported UUID correction `7e3c430` in its separate candidate, then
  a static-ENet/PIC link blocker. Neither build fix is imported into this checkpoint.
  No CMakeBoost,
  SimpleWeb, Motion helper or borrowed dependency tree modified here.
- Exact command/log evidence: `/home/bazzoozza/.bb/thread-storage/thr_z5wceyp8m4/checks/`.
  Cheap compiled artifacts: `/run/media/bazzoozza/Games/bb-targets/wolf/MWOW9/`;
  `cheap-results.json` records all four successful compile/run commands and logs.
  These are qualified syntax + isolated tests, **not** a native full link/build pass.
  No GStreamer pipeline, ENet listener, device, Docker daemon, game or Wolf main run.
- Full link, native scope execution, sanitizer integration and independent review
  remain gates. Manager requested this source checkpoint before the heavy slot.
  Exact token usage is unavailable; no fabricated usage report submitted. Manager
  owns the MWOW-9 lease. No merge, push, deployment or additional agents.

## Boundaries for subsequent owners (not implemented here)

1. **Consumer runtime:** keep these scope types/fields; add owned per-kind admission,
   cancellation, bounded ping wait, GLib-context commands and callback/resource drain.
   A scope check is not a serialization fence. Audio participates even when muted.
   Repeated ANNOUNCE, in-flight callbacks and stop-before-registration remain gates.
2. **InputSeat:** retain one MVP Xbox device; map one configured wire controller without
   rejecting protocol slots 0..15 globally. Use the same serial owner ticket for ENet,
   API and lobby routing; revoke admission, finish in-flight writes, neutral, then grant.
   Bind feedback to the current peer. Existing inputtino void setters do not prove
   kernel delivery; require a checked dependency seam or honest fail-closed policy.
3. **Device executor:** destination RunScope is already in every job. Add immutable
   container/device generation, bounded count/bytes queue, reserved control lane,
   FIFO completion and Unknown/quarantine outcomes. Current unplug can still perform
   synchronous filesystem/Docker work; run identity alone does not solve R5.
4. **Integration:** serialize resume/member replacement and side effects. Current
   snapshot checks + compare-and-replace prevent ID remapping but do not prove an
   atomic handoff of shared input/display resources. A queued device operation within
   the same persistent run still requires the new owner/device-generation gate.
   Lobby membership retains a snapshot across resume; run-authorized cleanup resolves
   that same run's current connection, but old-vs-new run retention/reconciliation and
   bounded membership storage still need the next stage.
5. Preserve MWOW-6 key registry, V2/legacy policies and all field guards; MWOW-7
   Docker Unknown, retryable stop and worker-handoff exception accounting. No generic
   singleton/all-routes prefix ownership, restart recovery, 16-pad neutral guarantee,
   measured 5s deadline, or bounded physical GStreamer/kernel completion is implied.

Manager may split the next consumer/input/device stages only after reviewing this
frozen interface checkpoint and assigning disjoint file ownership. This document
is the handoff contract for the foundation, not acceptance of R1/R6/R9.

## F6/F7 regression checkpoint (RED seam)

Independent review of `8e930ba` identified two sequential routing defects:
consumer startup after a retained-lobby resume selected the source run instead of
the lobby (F6); numeric membership alone allowed a replacement run's devices into
that lobby and suppressed its own queue (F7). Manager's native combined `5e465404`
passed full compilation and six isolated suites; those suites did not cover these
triggers. The old link-pending section above is historical evidence for 8e930ba,
not the current status of Manager's combined baseline.

This regression checkpoint deliberately preserves the old routing decisions:
`state::get_lobby_for_session` delegates to numeric membership and
`state::prepare_stream_consumer` selects the source run. New `[scope][routing]`
cases are therefore expected RED. No passing runtime result is asserted here.
The next commit changes these decisions without weakening the assertions.

Production seams (all in `state/sessions.hpp`, not test copies):

- `get_lobby_for_session(lobbies, StreamSession)` is shared by both lobby device
  relays, own-queue suppression and explicit UI-combo leave.
- `prepare_stream_consumer<VideoSession/AudioSession>(request, sessions, lobbies)`
  runs after RTP ping and before streaming. It validates the captured current
  connection/run, replaces any incoming `producer_scope`, and returns an optional
  prepared consumer. Both audio/video startup paths call this same function.
- `queue_own_session_device(event, sessions, lobbies, queue_scope, queue)` is the
  actual own-queue adapter used by the Moonlight event callback. The tests execute
  it directly without starting a session to create that queue.
- VideoSession and AudioSession now have hidden `producer_scope`, separate from
  the connection's original `run_scope`. Initial named interpipesrc binding and
  ProducerBinding retain this selection; no raw-ID alias was restored.

The three new cases cover admitted lifecycle membership plus real pause handlers,
new audio/video preparation after resume, stale/replacement requests, and five
routing variants (resumed same run, replacement run, missing retained member,
missing committed projection, null lobby scope). Device tests execute actual
lobby event handlers and inspect both the real TSQueue and the emitted unplug.
The legacy API relay fixture now includes its retained member snapshot; its old
assertion remains. Input-object identities and absence of implicit leave are
checked; these synthetic joined-state fixtures do not create a Wayland display,
exercise physical input, or prove the pending InputSeat/neutral/drain contract.
No CMake/build dependencies or other future-stage implementation is changed.

## F6/F7 correction checkpoint (GREEN source)

RED predecessor: `8241631b08efe5846f34fc072c27e4facff8d70f`.
The correction is confined to the two production decisions in `state/sessions.hpp`:

- `get_lobby_for_session` now requires a valid source connection/run, a valid lobby
  run, the retained `members[source.session_id]` snapshot with the **same RunScope**,
  and its committed connected-session projection. A new connection of the same run
  is allowed; a different run reusing the ID, absent member, absent projection,
  or null lobby scope is not. The ID-only lookup helper is removed entirely.
- `prepare_stream_consumer` selects that confirmed lobby's RunScope, otherwise the
  source's own RunScope. Both audio/video use this decision after ping; the prepared
  value and the actual streaming ProducerBinding retain the selection. The original
  connection/run identities are unchanged. A supplied producer_scope is overwritten.
- Plug relay, unplug relay, own-queue suppression, and UI-combo routing all use the
  same guard. A mismatched replacement run gets no lobby scope; the own-queue adapter
  instead enqueues the unchanged event into that run's queue. No silent leave or
  raw-ID sink alias was introduced. The regression assertions are unchanged from RED.

Ownership/interfaces: only `events/events.hpp` (hidden selected-producer field),
`state/sessions.hpp` (three seams), `sessions/moonlight.cpp` (after-ping/queue adapters),
`sessions/lobbies.cpp` (shared membership decision), `streaming/streaming.cpp`
(initial selected binding), `tests/testConnectionScopes.cpp`, `tests/testWolfAPI.cpp`
and this document. No CMake, Boost, ENet, Beast, security guards or Docker Unknown
logic changed. Scope type interfaces from 8e930ba remain unchanged.

Qualified syntax checks use Manager's current native compile database with source
paths remapped here; UUID already exists in those native flags, no extra include
qualification is added this time. These are not a native build/link pass. Root owns
RED/GREEN native builds and suite execution. Until that evidence and independent
review arrive, this is a **review candidate**, not a measured GREEN test result.
Artifacts: thread-storage `thr_z5wceyp8m4/checks/f6-f7-red/` and `f6-f7-green/`.

This fixes the sequential membership/producer decisions in F6/F7. It does not make
separate snapshot reads atomic with concurrent join/leave/resume or GStreamer side
effects. Consumer cancellation/drain, serial InputSeat and neutral delivery, feedback
rebinding, bounded device executor and retained-member reconciliation stay deferred.
The tests use committed membership records and input-object identity markers; actual
Wayland input redirection and audio/video transport are not exercised here.
