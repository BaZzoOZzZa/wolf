# MWOW-9 checked Xbox backend — atomic step 1

Scope: additive dependency/backend only, unreachable from legacy runtime. No InputSeat,
GameACL, PeerToken, grant, coordinator, ingress, API/UI or product enable. See the
appended decisions in [the design](MWOW9-input-seat-design.md); user choices do not
extend this implementation scope. Baseline `5eb6eb16fa5544dbd5bdef161f25eb1ad2593030`.

## Ownership and interface

Tracked patch adds only `include/inputtino/checked_xbox.hpp` and
`src/uinput/checked_xbox.cpp` to inputtino full pin
`fd136cfe492b4375b4507718bcca1f044588fc6f`. Existing source, API, Xbox/Switch/PS state
and motion patch are unchanged. New cpp is picked up by the existing native glob;
header installation already includes the include directory. Wolf CMake composes
motion first, checked Xbox second, and separately verifies both. Source overrides
are verify-only. The checked driver validates five unchanged baseline files, absence
or exact post-hashes of both additions, and the full Git pin when .git exists.
Unknown/partial inputs are refused before mutation. Archived fixtures are test data,
not FetchContent overrides or build inputs.

`inputtino::checked::Xbox::create` is the only new device-opening entry point; nothing
calls it from Wolf. It reuses the actual Xbox capability constructor and retains its
uinput handle/fd. `adopt(Device, Io, Feedback, ThreadFactory)` is the same owned
production setup path, with every syscall/factory explicitly injectable for tests;
no missing-hook fallback opens a device. The injection contract requires Device.owner
to own its fd and the factory to obey std::thread all-or-nothing startup. Injected
operations/captures must outlive their calls; tests enforce owned teardown. No raw
peer or grant semantics are present. No threads start on library load.

`DeviceGeneration` is a fresh retained opaque identity. `apply(expected, Frame)` and
`neutral(expected)` use the same serial writer. Twenty exact input_event writes:
11 button releases/states, two hat axes, four stick axes, two triggers, SYN_REPORT.
Neutral force-writes everything, even a zero cache. Y inversion is widened/clamped.
Each write returns count+captured errno; only exact size advances accepted_events.
Zero/short/oversized result is Unknown, negative errno is Failed, thrown IO is
Unknown, wrong/null generation and stopped/invalid state are Rejected. Receipts
carry actual attempted/accepted counts and sequence. Sequence/cache advances only
after the final exact SYN. Failed writes poison normal apply; an explicit neutral
repair attempt may succeed, but does not clear poison or authorize any grant.
No remaining-byte replay, no automatic retry, no libevdev-return-0 success inference.
`KernelFrameAccepted` describes the software syscall contract only, not kernel state
readback, Wine slot or game consumption. Tests never make real device writes.

The separate listener state installs its immutable callback before thread start.
It polls with 10 ms requests; this is a policy, not a kernel latency guarantee.
It supports FF_RUMBLE uploads/playback/gain/erase with a 16-effect cap; other effect
uploads explicitly return -EINVAL. Legacy constant/periodic/ramp emulation is not
changed or advertised as supported by this new handler. Reusing the capability
constructor means those upload requests can still reach the handler and be refused.
Callback/read/poll/ioctl exceptions or terminal errors mark failed and initiate stop.
Transient read EAGAIN/EINTR and poll EINTR remain cancellable. The callback copy stays
owned through invocation; no mutable callback swapping during handoff exists here.

`request_stop` is terminal backend retirement, not InputSeat revoke: a future handoff must retain this backend/listener and mute its router while invoking neutral through the serial owner. `request_stop` is nonblocking. A reserved in-flight frame may finish. The listener's
cleanup waits for that writer, destroys callback/IO captures and the device owner,
then signals completion. `retire_until(deadline)` on the creating owner thread joins
only after completion; Unknown retains the object/thread/resources and denies future
writes. A slow callback, write or device destructor cannot be reported as Retired.
A wrong thread receives WrongThread. Destruction cancels and actually joins; it may
block and must never run on its own callback thread. Callers must retain Unknown
objects in their future quarantine; this backend does not implement that supervisor.
`listener_status().failed` remains diagnostic even when retirement succeeds: release
is not successful operation. There is no detach, force-close or device replacement.

Wolf `control/checked_xbox_backend.*` is a thin owned adapter forwarding generation,
frame, neutral, stop and retirement. It requires a non-null owned handle. No core
exposure changes are necessary. Non-Linux translation units remain empty.

## Tests and boundaries

`wolf-checked-xbox-tests` uses actual dependency encoder/writer/listener and Wolf
adapter, with fake write/poll/read/ioctl/thread factory. No device-opening factory,
real device fd, real ioctl, Wolf/Gst/ENet startup is invoked. Coverage includes every
failure index including SYN (EAGAIN/EINTR/EIO/zero/short/oversized/exception), full
mapping/extremes/cache progress (including individual one-hot wire buttons), explicit neutral after partial state, same-fd ABA,
null/absent handles, thread start/allocation rollback, stop-before-entry, callback
capture/in-flight retirement, stop during poll, in-flight writes, delayed device
destruction, listener faults/transient IO cancellation and wrong-thread retirement. Test gates and writers
are released/joined even during assertion unwinding. No model mirror/global new hook.

Native evidence and patch-driver fixture results are recorded separately after
execution with exact source SHA/argv/log/exit/hash; this document alone is not a PASS.
No assertion in existing suites is weakened. All old consumer build metadata and
F8/F9 raw evidence remain immutable; build root is new `MWOW9/input-backend`.

Host kernel source provenance, actual uinput acceptance, physical/game consumption,
Wine XInput mapping, other-local-game isolation, bounded physical termination,
InputSeat/feedback send fences, checked mixed input and bounded Docker executor are
not proven here. Inherited eventbus noexcept/EAGAIN and supervisor allocation failure
remain separate gates. Backend review is required before stages 2/3 or enabling it.
