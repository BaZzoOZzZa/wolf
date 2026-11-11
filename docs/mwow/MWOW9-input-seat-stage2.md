# MWOW-9 Stage2 — disabled InputSeat and joint coordinator

Assignment supersedes historical step1-only restrictions only for these internal
classes. No runtime caller, public ingress, GameACL implementation or enable switch
is added. Baseline328e159; game-complex owner, no delegation.

## Design delta before implementation

A server-owned InputSeatDomain reserves its sole slot before factory work; it never
replaces a failed/Unknown seat. One persistent checked backend belongs to exact
RunScope L. A shared gate contains retained immutable grants and bounded queues,
not the backend. Backend callback captures a stable router owning that gate;
there is no cycle back to backend/seat. The factory receives that callback before
starting the backend listener. Inventory must explicitly attest fresh ProtectedPadOnly,
exclusive routing and no preexisting non-pad backend/queued compositor input.
An empty held-state set is insufficient. These attestations remain trusted caller
preconditions until Stage3 enforces all routes.

The input worker reserves each complete frame under the gate, executes IO outside
it and retires the reservation under the gate. FIFO256 /64KiB includes command
storage plus owned payload capacity; overflow or allocation failure closes input
and feedback and latches neutral outside the normal queue. No release coalescing.
Only the selected wire slot (default0, configurable0..15) accepts pad snapshots;
all other kinds/extensions are explicit refusals. Legacy wire protocol is untouched.

Router capture stamps device/grant/peer at intake. Enqueue and send reservation
recheck that exact grant under the same gate. A send reservation has an explicit
retirement acknowledgement; losing it cannot prove a completed send. Revoke drops
queued input/feedback and waits for reserved IO/sends. Neutral then runs regardless
of consumer completion. It never calls terminal backend request_stop during transfer.
A future ENet owner executor must retain the reservation until actual send retirement;
this stage does not claim the existing ENet outbox is fenced.

Coordinator reserves an immutable shared TransferHandle before revoke, captures the
old grant and immediately saves ConsumerRuntime::revoke(old.connection). It retains
the actual runtime and ticket through pruning. One pending transfer; duplicate
identity returns the same handle, competing request Busy. Fixed supervisor checks
one monotonic5s deadline and exact ticket, input/send fences, checked20event neutral,
backend generation and trusted current authorization/binding/lifecycle predicate.
Grant commit and feedback publication share the gate. Candidate disconnect/auth
change or any failure/Unknown closes the transaction permanently; late completion
cannot grant. Prepared consumer admission is an internal gate only, not ANNOUNCE
wiring. Initial fresh admission requires worker neutral plus trusted validation;
there is no missing-ticket exception for a transfer from an existing grant.

Resource cap per domain: one backend listener, one input worker and one coordinator
supervisor, including Unknown. No per-packet/per-transfer thread. Shutdown is owner
join (possibly physically unbounded); no callback blocks waiting for drain. Timer
policy is a5s decision threshold, not measured scheduling/driver latency. Supervisor
and validator are internal nonblocking boundaries; validator must not reenter gate.

Implementation ownership: new control/input_seat.{hpp,cpp},
sessions/input_handoff.{hpp,cpp}, tests/testInputSeat.cpp, minimal isolated target,
this document. Existing backend/scope/ticket interfaces stay unchanged. Tests use
real checked backend with injected IO and real ConsumerRuntime Work resources.
Stage3 all-route wiring/auth/ENet executor and separate device executor remain gates.

User restriction received during Stage2: WoW must not be launched without a new
explicit permission. Existing game and Wine prefix remain untouched. Future
physical checks should prefer local games, but this assignment authorizes only
software injected IO and no devices/games/daemons/system changes.

## Resolved interface decisions

Manager confirmed: joint handoff requires BOTH fresh ConnectionScope and fresh
PeerToken. Same-Connection request is rejected before old revoke or neutral;
a new peer token alone cannot reopen ConsumerRuntime's revoked scope. Numeric
ID reuse is allowed only with fresh retained allocation identities. ConsumerRuntime
is unchanged. Peer-only notifications must identify the exact admitted grant/peer.

Manager authorized only one additive CheckedXboxBackend forwarding method:
listener_status() const -> Xbox::listener_status(). Commit revalidates failed/stop
snapshot; supervisor quarantines subsequently observed terminal failure. This is
not an atomic guarantee against an async failure immediately after observation.
The wrapper header/cpp are the only additional owned paths; dependency unchanged.

`admit_prepared` is the execution gate: validation plus actual ConsumerRuntime
admission run in the seat serial domain. `prepared_admission` is inspection only,
not a reusable permission to start a consumer outside the gate. Runtime Current
and authority callbacks must not reenter the seat; no gate is held across drain
waiting or device IO. Command budget includes the one reserved in-flight command.

Feedback has at most one outstanding send reservation in addition to its fixed
256-entry queue. A lost acknowledgement holds this slot and blocks the fence;
new sends cannot accumulate behind an Unknown executor. Fresh bootstrap and
prepared admission also check the backend listener terminal snapshot.

ConsumerRuntime's retained job run is the **source** run S/T (its stable ID must
match ConnectionScope's ID), while the persistent pad targets L. `admit_prepared`
passes the captured source run; Stage3's media Work must retain F6/F7 producer L
selection. No numeric lookup supplies either authority. Initial test failure caught
this mapping error via the existing Stale guard; the guard was preserved.

Authority/connection/binding revocation notifications must enter the same serial
protocol (`candidate_gone` for the exact candidate or `InputSeat::revoke` for an
exact current grant/peer). A notification racing commit also closes the matching
just-published grant. External GameACL mutation without this integration is not
claimed atomic; implementing that protocol is a Stage3 gate. A Granted transfer
handle records a past decision, not perpetual permission: every input/send/consumer
admission checks the current gate again. Late terminal backend failure revokes
future admissions; no claim of impossible failure-after-observation.

Both owned thread launches have a narrow trusted all-or-nothing factory seam.
Throwing or returning a nonjoinable thread cannot publish a usable seat/coordinator;
source tests exercise actual rollback, backend resource destruction and cap denial.
A factory that secretly detaches work violates this internal contract and is not
supported. The default is std::thread; no threads launch at library load.
