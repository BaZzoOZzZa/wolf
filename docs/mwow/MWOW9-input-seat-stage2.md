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


### F12 correction checkpoint (review required)

Scope: prepared Work ownership plus validator capture retirement at bootstrap,
previous TransferHandle replacement, and transaction-constructor rollback.
Admission and validation must remain serialized. Callable invocation is trusted
nonblocking/nonreentrant; destruction of captured resources has no such contract.
The RED fixture uses actual Seat/Coordinator/ConsumerRuntime and checked injected
IO. A sole capture blocks in its destructor; concurrent current/revoke/poll and
neutral must progress. Only after that progress is observed does the destructor
attempt direct current/revoke reentry. On RED this last attempt is suppressed to
avoid a permanent self-deadlock; all test gates release before owned threads join.
Bootstrap checks gate progress, not another worker operation while that same
worker is executing cleanup. Original fifteen cases remain intact.


F12 ownership law: `admit_prepared` creates one retained Work object and a shared
forwarding callable **before** acquiring the seat gate. The outer strong owner is
declared before the lock, so every local return/unwind destroys the lock first.
Runtime rejection (including Duplicate/Revoked/Busy/Capacity/Stale), exceptions,
and a worker finishing immediately cannot remove that outer owner. If the worker
outlives admission, Runtime's original capture-retirement/join law still applies.
No external gate is removed from actual Runtime admission. No cleanup thread or
recursive mutex is introduced. Payload cleanup can delay the caller after unlock;
it does not promise bounded physical resource destruction.

`request` similarly retains the single actual validator behind a forwarding
callable before locking. Partial TransferHandle construction (including prepared
grant allocation failure) only destroys shared wrappers under the gate. An old
pending handle is moved into an outer retirement owner before replacement; it is
released after unlock, including subsequent revoke exceptions. Bootstrap moves
its validator to a worker-local retirement owner outside the completion lock.
Serial invocation/commit semantics are unchanged. Bootstrap resource cleanup can
still delay that same worker's next operation, but cannot monopolize the gate.

Pre-gate payload/wrapper allocation failure propagates to the trusted caller with
no admission/revoke mutation; captures unwind outside this gate. No claim of full
ingress exception safety. The actual throwing Clock seam exercises transaction
argument preparation rollback; no allocation fault injector/global new override
was added. Mid-constructor prepared-grant bad_alloc is covered by ownership and
C++ destruction order, **not** claimed as an executed allocation-fault test. Fast
successful Work is executed, but the scheduler's exact pre-unlock worker return
interleaving is not forced; the outer-owner proof covers that interleaving.

Native RED at 61b34d6880af5c75b2a748195bbc2816631d0d00: four cases,59 assertions,
ten expected progress/reentry failures, normal Catch exit42. Early refusal and
fast-success paths passed their checks. Five cleanup paths failed: actual Runtime
Duplicate, actual Runtime Current exception, bootstrap validator, previous granted
handle replacement, and throwing transaction preparation. Original test bodies
are preserved. Final GREEN/evidence are recorded separately in the F12 report.
No ConsumerRuntime, dependency, public interface or ingress changes. Remaining
Stage3/physical gates and the prohibition on launching WoW are unchanged.


F12 completion refinement: Runtime's ticket proves retirement of Runtime-owned
references; a fast worker return/cancel can precede retirement of the admission
call's temporary owners. The seat now reserves **one** bounded cleanup slot,
retaining the exact grant/connection, under the same gate immediately before
actual Runtime admission. A second admission while occupied fails Capacity and
quarantines, without calling Runtime or creating another worker. close/revoke do
not clear this slot. An inconsistent pending/owner pair fails closed. The joint
grant waits this fence in addition to the real saved ticket and existing fences;
neutral does not wait it. The same5s decision deadline applies, and a late cleanup
ack cannot reopen Quarantined.

`AdmissionRetirement` is declared first, followed by retained Work, forwarded
Work, and the owned optional hook. Work/hook function parameters are emptied with
swap (no assumption about moved-from std::function ownership). Reverse local
lifetime order destroys hook captures, forwarded references, and outer Work
references outside the gate, then acknowledges under a short lock+notify. Runtime
may keep its own Work reference longer, covered separately by the saved ticket.
Rejected/throwing Runtime paths use the same RAII obligation. Pre-gate allocation
failure has no published Runtime work/obligation and propagates without mutation.
Early identity refusal publishes no consumer; cap overflow is terminal quarantine.

Optional `before_cleanup` is an injected internal test synchronization seam,
default empty, invoked only after actual Runtime result and unlock. It is neither
a public/wire API nor a completion proof. A throwing hook quarantines/refuses;
existing exact poll/revoke accounts for any real Runtime work. Hook invocation and
its captures must retire before slot ack. No new executor/thread, detach, or
ConsumerRuntime/ticket semantic change. Destructors must obey the normal C++
nonthrowing destructor contract; no guarantee against physical hangs is invented.

Validator boundary differs: bootstrap/previous-handle cleanup can finish after
bootstrap publication or the next transaction has been installed. Validators are
trusted authority predicates, not owners of this admission call's Work obligation.
Their capture destruction is allowed to reenter gate methods after unlock; it is
not an extra neutral/media completion proof. Arbitrary externally shared owners
are not covered by the admission fence. Lifecycle notifications must still carry
exact grant/peer identities; a destructor cannot infer authority from numeric IDs.
The protected backend remains retained across transfers. No Stage3 enable or
physical input delivery/isolation is claimed.

Separate fence RED at b418b080b5a4bc514fc4f6d8ce4731d02c6faa8a: two cases,
71assertions,19expected failures, normal exit42. Actual real joined ticket is true
while hook invocation, hook capture destructor and sole outer Work destructor are
separately gated. Healthy release must permit grant; cancel-before-ping plus fake
deadline must quarantine and refuse late auto-grant. Additional overflow and
throwing-hook paths exercise real Runtime cancellation. This second RED includes
only a test-unwind declaration-order correction in the first F12 cases (atomics
outlive joining threads); no original15case bodies/assertions were changed. Each
RED/GREEN pair has its own exact test-byte hash in the evidence.
