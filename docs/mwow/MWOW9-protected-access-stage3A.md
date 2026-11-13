# MWOW-9 Stage3A — disabled protected access facts

Baseline: `de391eb305cd213eecf31a98b1d196d2b5ab21ec`, accepted
[Stage3 route plan](MWOW9-stage3-ingress-plan.md). Stage3A implements only internal
facts/evaluation. Independent review is required before Stage3B/3C. Nothing calls
these classes from runtime ingress; no configuration parser, setting, endpoint,
Seat construction, device, listener, runner or user-visible capability is added.

## Trust and exact identity

`VerifiedIdentitySource::verified_leaf()` is the trusted integration seam. Its
future adapter must obtain the presented leaf from a successfully verified TLS
connection and the paired leaf from the current authoritative pairing store.
The returned X509 objects must be retained and stable during capture. The seam is
an internal C++ contract, not a cryptographically unforgeable object against other
trusted in-process code. A test implementation does **not** demonstrate TLS key
possession. No wire boolean, numeric client ID, first packet, PIN or pairing event
constructs an authorized principal here.

`Principal::capture` first takes bounded immutable DER snapshots and then reuses
the public `x509::verification_error` exact-cert helper (MWOW-6, unchanged).
That helper compares complete certificate DER, independently of certificate dates;
it does not perform TLS. A different signed leaf with the same subject/public key
is a different identity and needs explicit operator approval. Existing crypto,
V2 replay/nonce/bounds, authentication and legacy behavior are unchanged.

`Leaf` uses real OpenSSL `d2i_X509`, `i2d_X509`, explicit-length memory BIO and SHA256.
The DER input must consume its entire buffer and equal OpenSSL's reserialization;
this is not a claim of a separate universal BER canonicalizer. PEM permits one
certificate followed only by whitespace, no ignored chain/junk. The bounded BIO
avoids using the existing `cert_from_string` helper's NUL-terminated BIO_puts on an
arbitrary string_view. No existing helper was modified.

An operator's `RuleView` explicitly maps one stable game key to one canonical
catalog appid and an exact leaf allowlist. No wildcard, title/path/prefix alias or
paired-all policy exists. SHA256 and legacy numeric indexes are lookup metadata;
complete DER equality is required. Different leaves sharing either index are
ambiguous and denied, including collisions with another configured game. The same
leaf under different numeric indexes and duplicate entries within one ACL also
reject the policy. One exact principal may intentionally appear in several games.
The function-pointer digest index seam allows deterministic collision tests; it
cannot bypass full DER comparison and owns no arbitrary callback capture.

## Store and revisions

`AccessStore` owns immutable current policy and at most one target fact, protected
by its private mutex. Policy preparation/hashing happens outside the lock; commit
is serialized. No external callback, I/O or arbitrary payload destructor runs
under this lock. Retired current records release outside it. Newly prepared
records have only bounded strings, DER references and internal identities.

Each accepted policy replacement creates fresh retained `ConfigRevision` and
`AuthRevision`; identical values do not resurrect old evidence. Invalid, empty,
ambiguous, oversized or failed policy preparation invalidates the current policy.
A running/Unknown target's game key/appid association cannot be changed by policy
replacement: conflict denies the policy but retains that target slot. Explicit
operator replacement can restore policy under fresh revisions; it does not grant
input, reconcile Unknown or reopen consumers.

`bind_target(game, appid, RunScope)` requires an existing configured mapping and a
non-null retained run. One active/Unknown slot cannot be replaced by another
allocation with the same numeric/string ID. Rebinding the same running allocation
creates a fresh binding/lifecycle fact, invalidating old evidence. A trusted
`set_lifecycle(exact TargetFact, state)` changes lifecycle revision; stale/foreign
handles are refused. Unknown denies evaluation and retains the slot. Unknown and
Ended cannot become Running implicitly. Explicit terminal Ended followed by a new
binding is a new lifetime. Ended is a **trusted asserted fact**, not this module's
proof of process/device completion; future integration must supply that proof and
may not clear quarantine merely to fit this interface.

`evaluate` returns immutable historical `Evidence`: exact principal, game/appid,
policy/config/auth revisions and, when bound, exact run/binding/lifecycle facts.
ACL-only evidence with no target is possible and explicitly is not a run grant.
`historical_allowed()` remains true after revocation. `is_current(evidence)`
compares only against this store's owned current policy/target and complete
principal DER; it accepts no caller-provided "current snapshot". A different store,
policy replacement, invalidation, rebinding or lifecycle revision makes old
captures noncurrent. Source/client S and destination L are never conflated.

This is a current **facts** check, not serial authority for an effect. TLS/pairing
revocation does not magically arrive here: the future trusted authority owner
must serialize invalidation and source revalidation with handoff/commit. A
previously captured principal cannot be treated as live TLS proof. A completed
store invalidation is visible to subsequent `is_current`, but a later explicit
operator policy publication is a separate operation; async configuration update
ordering and real pairing notification are not implemented. No lock in this
module spans external commit, ConsumerRuntime admission, Seat or ENet send.

## Resource policy and failure boundaries

Finite proposed internal limits (not changes to legacy wire protocol):

| Extent | Limit / validation before allocation or copy |
|---|---|
| Certificate | 16 KiB DER; 24 KiB PEM input; explicit byte length |
| Stable game key | 1–64 ASCII alnum, `_`, `-`, `.`; neither `.` nor `..` |
| Catalog appid | canonical positive decimal uint32; no leading zero |
| Run diagnostic ID | nonempty, at most 64 bytes; authority remains allocation |
| Policy | 32 game mappings, 64 total pin entries, 256 KiB aggregate DER |
| Target facts owned by store | one current slot including Unknown |

These defaults apply only to this disabled model; unsupported legacy catalog
values are not silently rewritten. Entry counts and aggregate DER are preflighted
before allocating/copying the policy. Repeated pins across games are charged
again. DER byte limits bound parser input, not OpenSSL RSS or execution time.
Old snapshots retained by callers are not a global memory cap. Multiple externally
constructed AccessStores/Leaves are not a process singleton: integration ownership,
all-routes accounting and retained-history caps remain gates.

No selection intent registry is needed for Stage3A. Max-one pending selection,
server-minted duplicate identity vs competing Busy, coordinator quarantine caps
and prepared consumer admission are still Stage3C responsibilities. No empty
registry is treated as proof of completion/readiness.

Policy `bad_alloc` returns AllocationFailure and invalidates old policy; other
preparation exceptions invalidate then propagate. Leaf/principal/target allocation
exceptions propagate without publishing partial objects. They are not an
exception-safe HTTP/eventbus ingress boundary. Tests inject an index exception
into the actual policy preparation path, not global operator-new failures; they
make no allocation-fault runtime coverage claim.

## Ownership and verification contract

Only new `sessions/protected_access.{hpp,cpp}`, new `tests/testProtectedAccess.cpp`,
this document and an appended isolated target in `tests/CMakeLists.txt` are owned.
Existing source globs compile the new class into wolf_runner; there are no runtime
callers. `wolf-protected-access-tests` compiles the same production cpp and links
real wolf::crypto/OpenSSL plus Catch2WithMain. No generic main/discovery is run.

Tests use ephemeral in-memory certificates, including a separately signed leaf
with the same key, real PEM/DER conversion and a large real certificate. No key or
secret is saved in Git. They cover missing/null identity, mismatched paired leaf,
exact approval, denied unconfigured game, numeric/digest collisions, malformed and
ambiguous policy, certificate replacement, store identity and revision ABA,
active binding conflict, lifecycle Unknown retention, stale fact refusal, count
and byte limits, preparation exception and completed revocation across threads.
The concurrency fixture calls the real store; no authority/state model is copied.

Native evidence must record actual configure/build/test argv, exit, logs, source
hashes and inherited sanitizer flags. Prior F12 metadata/evidence is snapshotted
before regeneration; old raw files remain immutable. New records use `3A-` prefix.
Expected final scope is 12 compile/link targets and 10 isolated software binaries;
results and first failures belong to the external checkpoint report, not an
unverified claim in this design document.

## Remaining gates

No remote GameStop authority is added; operator stop needs a separate explicit
future capability. Enabling remote stop would require a user decision. Unix input
without explicit existing-grant delegation remains default-deny in the future
protected path; there is no new powerful injection endpoint here.

The approved fresh opt-in ProtectedPadOnly policy and selection of already-running
WoW by a client preauthorized **for that game** remain product requirements.
Stage3A neither enables them nor routes launch/resume/ANNOUNCE/ENet/API input.
GameACL current-store notification, authenticated selection integration, serial
revocation/commit, all-routes PadOnly checks, real ENet owner-thread send retirement,
consumer exact-ticket/F12 cleanup fences, singleton/prefix aliases, device
attachment receipts/bounded DeviceExecutor, reconciliation, kernel/Wine delivery
and isolation remain unimplemented or unproved gates. Existing accepted Stage2,
MWOW-6/7, lobby F6/F7 and consumer semantics remain unchanged.

WoW must never launch without new explicit user permission. The current game and
Wine prefix remain untouched. Future physical tests should prefer local games;
this software-only step does not authorize those tests or prove local-game/MVP
readiness. No Penpot/UI work is included. Confidence in the bounded facts contract:
97%; security integration requires its own review and actual ingress tests.
