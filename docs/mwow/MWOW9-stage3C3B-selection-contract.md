# MWOW-9 Stage3C3B — current pairing publication and protected selection

**Proposal only.** Source base `55ef2766411aca1c1c67e187c0cd1705dff8957c`, WHOLE
`bda08506e08048d0bde30913c02eb0d414edb7d6`, SRC `128deed85c50f6d6c86e180f4751813268d81b08`.
No production/test/config change, compiler, runtime or new verification run in this checkpoint.
Confidence: 99% in cited source ordering; 95% in the proposed split/locking. Protocol retry behavior,
real TLS possession and provisioning remain explicit unproved gates, not implementation assumptions.
This is an incremental contract over [3C §3/5/6 and §12–13](MWOW9-stage3C-integration-contract.md).
Evidence: [inventory](/home/bazzoozza/.bb/thread-storage/thr_z5wceyp8m4/checks/3C3B-plan/inventory.json)
and [numbered ranges](/home/bazzoozza/.bb/thread-storage/thr_z5wceyp8m4/checks/3C3B-plan/source-ranges.txt).
All paths below are relative to `src/moonlight-server/` unless marked otherwise.

## 1. Earliest dependency: pairing publication is not yet current authority

`protected_identity.hpp:33–85` seals immutable DER and a caller-supplied PairingRevision. It deliberately
cannot prove that revision is current. `rest/servers.cpp:99–104` still calls a first-match legacy paired
lookup; it neither uses that seal nor retains a current pairing publication. `state/configTOML.cpp:410–470`
updates the public paired atom BEFORE TOML save; save results are not checked by these void mutators.
`api/endpoints.cpp:104–150` mutates nested profile apps before entering update_profiles. A guard added
only to persistence/update_profiles is too late. No pairing epoch may be minted per HTTPS request.

Propose ONE `PairingMutationOwner` for the configured paired atom/persistence domain, with one registered
protected destination L in this MVP (including Unknown). It is a publication/mutation sequencer, NOT
an access evaluator. It owns a data-only immutable `PairingPublication` containing an owner-minted
PairingRevision, sealed PairedSnapshot, publication identity and exact source snapshot; max one current
plus one preparing/Unknown publication. The initial tuple is created once before protected registration
from the stable startup snapshot; subsequent epochs are minted only for checked versioned updates.
It must not perform `load(); fresh();` at a request boundary. Caller-owned historical tuples stay historical.
Caps cover owned publication records, not external config graphs/process RSS; retain only one exact source
snapshot for mutation comparison, not a history. Charge any copied settings separately before allocation.
An empty paired list is an explicit ClosedEmpty publication: current PairedSnapshot::seal rejects empty
input (`protected_identity.cpp:24–27`). Do not fabricate a successful seal/principal for last-client unpair;
close authority and publish absence of capturable identity, with later recovery still separately gated.

The registered runtime retains the real authority/Seat/shared Runtime, with no back-reference to this
publication owner. Register-before-ingress and no raw config mutation while registered are enable gates.
For one protected domain, conservatively close it on any paired-list/settings publication change,
including unrelated additions; no attempt to keep a mismatched pairing epoch live. This trades update
availability for correctness. Existing authority close is sticky quarantine: neither successful pair
update nor facts publication restores input. Seamless policy-update recovery is NOT promised.

### Proposed minimal interfaces (internal; not public API or wire types)

```cpp
struct PairingPublication; // private construction, immutable bounded data only
struct PairingUpdate;      // exact owner/token/sequence/expected publication; no callback payload
class PairingMutationOwner {
  PublicationHandle capture_publication() const; // absent while Closing/Preparing/Unknown
  BeginResult begin_update(OperatorCapability, PublicationHandle expected); // reserve1 + actual close
  CloseResult close_exact(OperatorCapability, PublicationHandle expected);  // reserved, no IO/queue wait
  PublishResult finish(UpdateHandle, PreparedPairingPublication, PersistenceReceipt);
  IdentityResult capture_https(const HttpsRequestPtr&); // default real getter, no wire-supplied Principal
};
// Narrow forwarding on the existing ProtectedRuntimeOwner, no public gate accessor:
UpdateResult begin_authority_update(OperatorCapability, FactHandle expected);
SubmitResult submit_authority_publication(UpdateTokenHandle, PreparedFacts); // occupied retirement cell
// Actual ProtectedAuthorityDomain capture overload, using its existing Seat gate:
PermitHandle capture_selection(SeatCandidate, Principal, FactHandle expected, PairingRevision expected);
```

`finish` accepts a private result produced by the real persistence adapter, not a caller boolean “saved”.
One busy writer includes queued, executing, rollback/cleanup and Unknown; checked monotonic sequence,
no wrap, no replacement, no second disk writer overtaking a blocked first. Max 64 sealed paired entries,
24KiB PEM/16KiB DER each, 256KiB PEM and DER per publication as 3C3A; current+preparing count separately.
F12 arbitrary resource retirement remains on the existing bounded worker/retirement cell, outside locks;
no new per-update thread, generic cleaner or unbounded closure queue. Explicit trusted close never queues
behind persistence/cleanup. A failed prepare/write leaves protected authority closed; rollback does not
restore an old grant. Partial/uncertain disk effects require reconciliation, not a fabricated success receipt.

### Ordering and serial proof

1. Prepare bounded command/token owners off gates. Reserve the one update under publication mutex,
   advance checked sequence and mark Closing (new captures denied); release mutex. A valid reserved close
   remains possible when busy and invalidates the latest sequence without releasing arbitrary owners.
2. Invoke actual runtime authority `begin_update`/`close_exact` outside publication/queue/config locks.
   Actual Seat gate is the effect linearization point. A grant just before that close is revoked by it;
   paired/config mutation cannot occur until actual close has returned for every registered affected domain.
   Stale/foreign/null operator or expected facts cannot fall back to current-global authority. Busy/error
   never means successful permission; preserve exact result and deny publication/mutation as required.
3. Only after close, prepare new list, seal and `Facts::prepare(...newRevision)` off gates. Apply the real
   persistence/memory mutation via the one writer; failures retain closed/Unknown. Existing raw mutators
   cannot be wrapped merely AFTER their atom writes. New close may cancel publication but cannot free a
   still-running writer slot or let another writer overtake it. A late physical write is possible; record
   it as stale/Unknown and require reconciliation, never grant from it.
4. Actual authority `publish_prepared` checks its retained token/latest sequence/expected facts under Seat
   gate, then retires payload outside locks (`protected_authority.cpp:210–247`). Only matching completion
   can publish the new coherent owner tuple; recheck the publication-owner sequence after returning. A
   close racing either side leaves captures closed. Publishing new facts never reopens the Seat.
5. HTTPS captures ONE owner-held immutable tuple, calls real 3C3A capture outside locks, and obtains the
   principal plus that exact revision. Currentness is checked again by typed authority capture, not an
   external validator returning true. The proposed overload compares expected facts/revision under both
   existing gate sections around off-gate evaluation (`protected_authority.cpp:116–145`). Existing Permit
   already retains facts/sequence; request, final poll/grant and media admission check that SAME current
   cell. No extra permission evaluator, no Seat→publication mutex and no “is_current(oldStore)” authority.

An update after HTTPS capture but before request/commit closes/increments the actual authority sequence;
old Permit fails there. An update after grant revokes that exact current route before the mutation. A raw
old peer notice still uses exact connection+PeerToken; it never receives operator update capability.
Identity/route inspections are data-only; don't recreate F16 internal strong TransferHandle observers.

## 2. Actual mutator coverage before enabling (all currently unbridged)

| Route and actual site | Required earliest guard / remaining work |
| --- | --- |
| HTTP `/pair` phase4: `rest/endpoints.hpp:298–318` → `state::pair`; PIN sources HTTP `/pin` (`servers.cpp:39–55`) and Unix Pair (`api/endpoints.cpp:31–50`) only resolve promise | Guard before paired atom update. Success response currently precedes state::pair: protected bridge must not report durable/current publication before actual result. PIN/pair completion remains no GameACL grant. |
| HTTP `/unpair`: `rest/servers.cpp:57–71` → cache-key lookup → `state::unpair` | Existing client-id/IP cache tuple is not protected operator/leaf authority. Resolve exact retained leaf and validate allowed mutation source; deny ambiguous/missing. Do not expand its power by treating an unauthenticated request as arbitrary operator capability. |
| Unix `/unpair/client`: `api/endpoints.cpp:63–88`, route `unix_socket_server.cpp:55–63` | First proposed real ingress subset. Existing first numeric match must become unique full-leaf mutation selection; close actual registered domain before `state::unpair` writes. Local API transport access is not remote input delegation. |
| Unix `/clients/settings`: `api/endpoints.cpp:598–635` → `update_client_settings:447–470` | Certificate is preserved by endpoint, but folder/UID/GID/controller policy can change protected binding/isolation. Capture exact leaf, close before any update; don't infer harmlessness from unchanged cert or numeric ID. |
| Direct paired atom writers `configTOML.cpp:410–470` | pair, unpair, update_client_settings all need one protected-aware mandatory mutation boundary when registered. Keep nonprotected behavior via explicit legacy path, not silent protected fallback. Prevent caller bypass of the routed endpoint. |
| Startup/migration/load `wolf.cpp:40–53`, `configTOML.cpp:209–238,287–295,378–407` | Initial owner tuple before protected registration. Targeted source search finds no live reload/SIGHUP path; future reload must close before replacing config. External file edits are not monitored; no claim they automatically revoke running authority. Reload/watch installation is outside this plan's first increment. |
| Unix Add/RemoveApp `api/endpoints.cpp:104–155`; Add/RemoveProfile `169–204`; `update_profiles:473–498` | Guard BEFORE nested apps atom mutation and reflector creation, not only update_profiles. Stable GameKey↔catalog↔RunScope must invalidate on deletion/replacement. Generated appid (`configTOML.cpp:123–127`) is not a retained binding identity. |
| Protected ACL/facts `protected_access.cpp:196–303` policy/bind/lifecycle methods; `protected_authority.cpp:14–37,147–247` | Internal classes only; no current operator config/parser/reload ingress. Future explicit exact-DER per-game configuration uses same versioned close/publish protocol; profile PIN is not ACL. Unimplemented configuration is an enable blocker, not a discovered existing writer. |
| HTTPS launch/resume/cancel `rest/endpoints.hpp:429–528`; Unix session add/start/pause/stop/input `api/endpoints.cpp:215–403` | Branch before legacy creator/registry replacement/devices/event emission. Protected remote Quit source-close only; GameStop needs separate operator authority. Pause is not RunEnded. Input old-scope delegation cannot borrow a new grant. |
| Lobby create/join/leave/stop `api/endpoints.cpp:415–572`; `sessions/lobbies.cpp:166–550` | Gate exact L and member binding before lifecycle/membership/device effects; F6/F7 scope guards retained. No ordinary Leave/stop_when_empty during transfer. PIN does not grant protected takeover. |
| GameStop/RunEnded handlers `moonlight.cpp:45–77`, emitted `190–191,250–251`; `lobbies.cpp:590–637`; shutdown `wolf.cpp:285–294` | Exact L lifecycle closes authority BEFORE removing session/lobby routes. Event registration order alone cannot establish cross-handler atomicity. Network disconnect/pause cannot acquire GameStop authority. |
| Lobby setup/producer/runner outcome + Unknown `lobbies.cpp:100–159,350–391,533–545`; `lifecycle.hpp:130–175,218–301` | Close on exact destination leaving ready/going Unknown, not only final RunEnded. Internal completion/deferred stop and stop_when_empty must enter the same exact lifecycle notification boundary without changing MWOW7 retention. |
| Direct events/runners/creators: Unix RunnerStart `574–596`, reflected app/profile construction (`events/reflectors.hpp:33–123`), input UI combos (`control/input_handler.cpp:417–439,692–700` → `lobbies.cpp:640–675`) | Preserve earlier full route inventory; no protected runner/device creation through these alternate routes. Generic event stream endpoint `api/endpoints.cpp:12–21` is SSE output, not arbitrary event-injection API. |

This table inventories current tracked C++ sources, not unknown plugins/external direct writers. Public
mutable atoms and third-party callers cannot be proven mediated by a doc or optional endpoint parameter.
Full writer enclosure/API restrictions and re-audit of all routes are mandatory before enable. External
config reload policy, protected provisioning and same-game/prefix alias isolation are still blockers.

## 3. Selection / ANNOUNCE after the owner bridge

Protected selection entry is existing verified HTTPS launch(appid), or resume only with one retained
exact selection for that TLS principal. Map owner-held immutable GameKey↔catalog appid↔already-running
RunScope L; ambiguous numeric/title/path/prefix mapping denies. Capture actual identity from §1's owner
publication, evaluate real Facts/AccessStore outside gate, then actual typed-cell permit at request and
commit. Pairing alone/PIN/first input never selects a game. Early pre-peer selection is a bounded historical
intent, NOT a SeatCandidate/Permit: SeatCandidate requires a real CONNECT-minted PeerToken.

One current + one candidate including Unknown, max one normalized selection intent, bounded payload
≤8KiB before copy, fixed Audio/Video descriptor cells ≤16KiB each/64KiB orchestration logical total.
Reserve before copy; Busy leaves healthy old owner untouched, expiry cancels only captured candidate.
Candidate TTL30s is a proposed decision policy, not measured latency; actual joint transfer keeps its
single 5s monotonic deadline from real Handoff.request, never reset per handshake phase/retry.

Transport-only candidate allocates fresh ConnectionScope and logical source RunScope S/T as appropriate,
retains destination L separately and owns crypto/correlators. Do not call create_run_session or
create_stream_session (`state/sessions.hpp:130–184` creates directories and mt19937 correlators), copy
legacy device/display handles, fire StreamSession/StartRunner/PlugDevice or start another game.
Use fail-closed CSPRNG in existing wire fields with bounded 8 collision retries, checking current/candidate
AND legacy live entries for fake-IP/ENet/RTP correlation uniqueness. No numeric/IP fallback for protected
candidates. Registry construction/publication must serialize collision checks, not check then race insert.

Retry key is canonical exact TLS DER+owner publication+game/L/binding+normalized existing rikey/rikeyid
and negotiation fields. Same retained intent returns same receipt, conflicting attempt Busy. No public
epoch, unbounded old-key history or second nonce registry. Same-key new-intent policy remains unapproved:
software may reject/hold an ambiguous new intent, but must not advertise client interoperability until
actual Moonlight reconnect/retry evidence establishes an implementable policy. This is a later material
interop gate, not a user UI preference or a reason to invent new wire generation fields.

Proposed `PreparedAnnounce parse_announce(RequestPacket, TransportDescription)` extraction from
`rtsp/commands.hpp:166–285`: bounded numeric/required-field validation, immutable V2 negotiation result
and A/V descriptions; no event publication. Legacy wrapper retains its existing emission path. Protected
wrapper binds exact candidate via `rtsp/net.hpp:get_session/start` replacement seam, negotiates its real
ControlCryptoState BEFORE key proof and stages each kind once; identical retry idempotent/conflict denied.
Never wait for granted consumer before ANNOUNCE, never treat unauthenticated ANNOUNCE as GameACL grant.
RTSP write lifetime is accepted; overlapping writes, Content-length/prev_read aggregate bound and
protected resolver's IP fallback removal need separately reviewed patches before packet interop.

Actual Loop CONNECT mints PeerToken (`control_loop.cpp:371–415`). Same loop resolver retains exact
candidate/session; existing decrypt/tag/structure/V2 direction/replay/1024-key cap prove transport key
possession only after negotiation. Pre-grant input must be rejected, not buffered/replayed. Only then
form SeatCandidate and obtain current typed Permit; queue actual ProtectedRuntimeOwner Preparation.
It calls actual request and saves old ConsumerDrainTicket before publishing candidate lifetime
(`protected_runtime.cpp:191–227`, `input_handoff.cpp:147–160`). No legacy same-client first-match registry
swap, no shadow Runtime or fresh ticket lookup. Bootstrap additionally requires real retained provisioned
pad/device generation and fresh PadOnly inventory; neither real peer nor provisioning is available just
because HTTPS selection succeeded. Initial enable remains blocked by that separate receipt/inventory gate.

## 4. Small next grants and actual-seam tests (not PASS claims)

**B1 — versioned publication bridge on actual authority, still no endpoint/config mutation.** Proposed
ownership: NEW `sessions/protected_pairing.hpp/.cpp`; narrow `protected_runtime.hpp/.cpp` forwarding and
existing retirement-cell submission; `protected_authority.hpp/.cpp` expected-facts/pairing capture overload;
NEW `tests/testProtectedPairing.cpp`, appended tests target, this doc append. No Seat/Runtime rewrite.
Test real sealed identity + actual authority/Seat/Handoff/shared Runtime: old publication under new expected
revision denied; close before/after grant; blocked F12 cleanup allows close/status/deadline; stale finish,
Busy/exhaustion/foreign tokens and same-value epoch ABA; facts-only publication never reopens. Inject only
owned persistence outcomes and checked backend IO in software, not permission/close booleans. B1 closes
real authority and prepares the exact publication protocol; it does NOT cover config writers by itself.

**B2 — first actual mutator slice: Unix unpair → protected-aware state::unpair.** Proposed ownership:
new bridge from B1; `state/config.hpp`, `state/configTOML.cpp` narrow unpair overload/extraction;
`api/endpoints.cpp`, `api/api.hpp` for explicit owned bridge dependency, relevant new test and CMake/doc.
No global registration in Wolf yet. Refactor common unpair preparation/persistence seam so both fixture
and actual handler use it; do not implement a parallel checker. Resolve unique exact paired leaf before
calling bridge, close first, then actual protected persistence result/atom update/publication. Check
rfl save/load outcomes; void unpair completion is not persistence proof. A software file adapter may use
owned fixture files or injected low-level IO; no real config mutation in tests. Required tests drive
actual Unix handler/persistence seam: no writer called before actual close, failure leaves closed,
concurrent update cannot overtake busy writer, stale completion cannot publish, numeric collision denial.
All other §2 routes remain explicit enable blockers. B1/B2 split is necessary to review the gate-bound
protocol separately from existing nontransactional IO/response behavior; neither enables partial routing.

**B3 — transport-only intent + pure ANNOUNCE staging** after B1/B2 review and mutation-route plan:
NEW `sessions/protected_transport.hpp/.cpp`, narrow rest endpoints/servers, RTSP net/commands, actual handler
tests/CMake/doc. No routing enable until full §2 closure. Test same-client old/new coexistence, duplicate/
conflicting attempts, capacity/aborted ANNOUNCE/TTL, malformed fields, no legacy creator/event/device calls,
no fabricated peer, and actual old ticket captured before lifetime pruning. `submit` acceptance is not
Granted/media ready. Exact file expansion for each grant must be checked against then-current source.

Next separate Loop/Work work still needs: one bounded owner-thread cancel slot with exact retained Pending
TransferHandle, old grant/connection/peer/device/L and saved ticket. That is INTERNAL F16 ownership:
account its full payload/destructor retirement under an additive bounded actual gate obligation before
later grant; don't external-exempt it or use status/empty queue/manual ACK. Popped/Unknown stays charged;
reserved auth close independent. Actual cancel_for_transfer plus owner reset/free/enclosing stack must
retire sends; NoOutstanding is historical, not CancelledReleased. Preserve F13 stale isolation/F14 cell
retention. Proposed owned files Loop/protected-feedback plus narrow Handoff/Seat accounting only after
concrete review, same ControlKeyRegistry/crypto, no second decrypt implementation.

Actual media Work must use source S/T job scopes and producer L (including destination audio sink), existing
streaming_* lifecycle and same Runtime via serial admit_prepared; failure after Audio revokes candidate
and retains exact ticket, no partial readiness. Periodic owned-loop driver calls owner.status/poll_deadline
outside all gates; it is responsive when driven, not physical5s completion if execution is stuck.

Lock graph: publication/queue locks exchange data then unlock before any Seat call, verifier, IO or arbitrary
capture release. Seat→Runtime only accepted admission; Runtime Current remains immutable transport-data
read without Seat/config/queue/user callbacks. Off-gate facts/Work preparation, gate rechecks and actual
retirement ACKs remain mandatory. Tests must include close racing both sides of final commit, pending
pair revoke, delayed payload retirement, old peer notices against new T, real replay dispatcher and no
input pregrant; software fixtures do not prove real TLS/handshake/OS delivery/authority configuration.

No WoW/game/prefix/device/network/service operation is authorized here. WoW requires a NEW explicit command
AFTER readiness; arrival is not a trigger. Future physical tests prefer local games under separate grant.
UI/HUD requires Penpot. Full allroutes/singleton/DeviceExecutor/reconciliation/isolation, config writer
coverage and protocol interoperability remain enable gates. No ready-by-arrival ETA or full MVP claim.
