# MWOW-6 — security corrections and verification

Status: review candidate, not accepted or deployed. Remaining end-to-end epoch/barrier work is tracked as **MWOW-9** by Manager. Base: `0e3710be36a6a7fa130f633b358d1b520a3cee9c`.
No Wolf/game/container/device workload was started. Changes are local to `feature/MWOW-6-security-fix`.

## Corrections

- **T1:** pairing authorization compares the full DER of the presented and pinned leaf certificate. No issuer/CA fallback, key-only match or permissive opt-in. Null and different certificates fail. An exact expired pin is accepted, preserving Moonlight's clock tolerance; TLS still supplies proof of possession of the leaf key. A certificate reissued under the same key is a different paired identity.
- **T3:** received control buffers carry their actual extent. Header/envelope lengths must match it, before sequence/tag/payload access. The tag is exactly 16 binary bytes, including embedded NUL. Decrypted control framing and each known input shape are checked before dispatch; typed values are copied from bytes rather than downcasting a fabricated base object. UTF-8 payloads are bounded to 32 bytes; controller IDs must be in the protocol's `0..15` range before any shift or indexing. Absolute mouse dimensions must be positive. Unknown input types fail. The same bounded handler protects Unix input (400 on invalid input). Local typed callers have a size-known adapter, not an unbounded `INPUT_PKT*` overload.
- **T4/V2:** RTSP DESCRIBE advertises `x-ss-general.encryptionSupported: 1` and `encryptionRequested: 1`; ANNOUNCE fixes `encryptionEnabled` once per launch. Wolf's existing `7.1.431.-1` appversion selects the Sunshine extension path in Moonlight. V2 uses the existing 12-byte IV: LE32 sequence, six zero bytes, then `HC` for host traffic or `CC` for client traffic. All encryption and ENet sends execute on the control thread. A bounded queue replaces cross-thread ENet peer access from gamepad callbacks. Each callback carries a connection-specific revocable sender.
- **Nonce lifetime:** a control-thread-owned registry uses the decoded 16-byte key as identity. Reconnect/resume does not reset that key's outbound sequence or inbound replay state. Sequence reservation happens before encryption; a failed encryption/send still consumes the nonce. A 64-bit internal counter refuses after `UINT32_MAX`, never wraps. The registry retains up to **1024 distinct keys**, without eviction; after capacity, only unseen V2 keys are refused, existing keys/streams remain usable. Pending feedback is limited to **256 messages**, each at most 128 bytes. The inbound replay window allows reordering across ENet channels within 1024 sequence numbers, rejects duplicates/old packets and does not wrap.
- **Legacy policy:** disabled by default. `WOLF_ALLOW_LEGACY_CONTROL=TRUE` explicitly permits legacy inbound only; outgoing rumble/LED/motion requests/termination are suppressed. The legacy 16-byte IV uses only the low sequence byte and has no direction separation. Legacy is **not cryptographically safe**, even with this server-side mitigation; it is not a completed GCM fix. No private wire format was invented.
- **T2:** nonzero ENet tokens must match exactly and unambiguously; an incorrect token never falls back to IP. IP selection is only for zero-token, explicitly negotiated legacy connections and only with one candidate. CONNECT does not resume a stream. A structurally valid authenticated packet is required before ownership/Resume; another peer cannot replace the current authenticated owner of a generation. Received packets are freed on every rejection path. A per-launch shared crypto object plus transport secrets/key bind the local connection generation. Stale input/feedback is rejected; raw ENet pointers never escape into device callbacks. DISCONNECT cannot emit a session-ID-only Pause.
- **T6:** owned response bytes survive async completion. Each socket has one queued composed write at a time; reads, writes, close, SSE subscription/list cleanup and timer actions execute on one Asio strand. HTTP SSE headers are enqueued before subscription becomes visible. The per-socket pending write limit is **8 MiB**; a slow/oversized response closes that connection. This limit covers the socket write queue, not arbitrary upstream request/event allocation. Aborted keepalive timers do not restart themselves. A 404 closes after its queued reply completes.

The GCM helper also sets IV length before handing IV storage to OpenSSL, validates key/IV extents and rejects short/long tags. This removes an overread with short IV buffers and avoids a zero-sized decryption VLA. Default helper IV length now follows the supplied buffer length; protocol calls pass their explicit wire length and retained legacy capture vectors agree.

## Provenance / interoperability evidence

Primary reference: [moonlight-common-c eac4c469c755dc4fcf6c4bac12e17c0d78881fb7](https://github.com/moonlight-stream/moonlight-common-c/tree/eac4c469c755dc4fcf6c4bac12e17c0d78881fb7):
[ControlStream.c](https://github.com/moonlight-stream/moonlight-common-c/blob/eac4c469c755dc4fcf6c4bac12e17c0d78881fb7/src/ControlStream.c),
[SdpGenerator.c](https://github.com/moonlight-stream/moonlight-common-c/blob/eac4c469c755dc4fcf6c4bac12e17c0d78881fb7/src/SdpGenerator.c),
[RtspConnection.c](https://github.com/moonlight-stream/moonlight-common-c/blob/eac4c469c755dc4fcf6c4bac12e17c0d78881fb7/src/RtspConnection.c),
[Limelight-internal.h](https://github.com/moonlight-stream/moonlight-common-c/blob/eac4c469c755dc4fcf6c4bac12e17c0d78881fb7/src/Limelight-internal.h).
Used as wire-format/negotiation evidence only. This is an independent implementation in Wolf's existing C++ code; no Moonlight GPL implementation/functions were transplanted. V2 golden ciphertext/tag vectors were generated independently with Python `cryptography` AESGCM using that wire IV and the existing Wolf capture key. Legacy captured vectors remain in `testControl.cpp`. Actual Android/Moonlight network interoperability has **not** been run.

Upstream PRs were inspected as references, not blindly cherry-picked:
[#566](https://github.com/games-on-whales/wolf/pull/566) at `24a33bb59561f4b08a80009679057ac0ccfd6ab3` (issuer gate),
[#570](https://github.com/games-on-whales/wolf/pull/570) at `5d773ec56cd8dbc8ef5d7fddafcd4251e8f0eaa1` (outer bounds/binary tag),
[#561](https://github.com/games-on-whales/wolf/pull/561) at `49524c6061f1bd233dafaf2e7f123d18b732624d` (async buffer lifetime).
The prior beta #558 bad-tag fail-closed behavior remains. Control reply structs are packed to their wire lengths (including rumble/motion), so native tail padding is not serialized; explicit byte vectors cover these layouts.

MWOW-7 owns `api/endpoints.cpp`. Its exact adapter commit `0794803960db47d0e730f4e3a43da6296a157e1d` was reviewed and cherry-picked with Manager authorization as `d39ef29` for bounded input/SSE subscription. No lifecycle/Docker changes were mixed in. Git created that adapter commit, but sandbox denial of `packed-refs.lock` left a residual `CHERRY_PICK_HEAD`. Final full-status verification detected it; `git cherry-pick --quit` with authorized metadata access cleared only the operation marker, preserving the verified commits and clean worktree. The patch was not applied twice.

## Explicit residuals / release gates

1. **MWOW-9: end-to-end generation/lifecycle.** Local generation checks are not an atomic barrier against session replacement and event delivery. Pause/Resume/Stop and IDR still use stable session IDs in downstream consumers. DISCONNECT no longer pauses consumers; this prevents the unauthenticated disconnect DoS but can leave a consumer running until explicit cleanup. External Stop no longer uses the old unsafe cross-thread ENet termination callback; generation-bound termination delivery must be restored in MWOW-9. Producer/lobby/runner consumers all need the same epoch contract, not only streaming callbacks. No full handoff, cleanup or input-neutral barrier guarantee is claimed.
2. Old joypads carried across resume retain old callback handles. They cannot send to a recycled peer/new generation; feedback may stay suppressed until devices/callbacks are rebound. Input generation checks alone do not neutralize held buttons or prove old consumers have terminated. MWOW-9 must cover this.
3. Registry state lasts for one control-server lifetime, is not persisted across process restart. A client must supply a fresh key after a restart; reusing a key across a server restart is not proven nonce-safe. A client resetting its V2 input sequence under an existing key is rejected by replay state, not treated as a fresh stream. Server checks cannot undo nonce reuse already performed by a defective client. At registry capacity, fresh streams require an operational reset with fresh keys; eviction is deliberately forbidden.
4. Unauthenticated RTSP negotiation/transport DoS outside this gate, administrative Unix authorization/secret exposure (T5), request size/time limits, complete server destruction lifetime, container/input isolation (T7), and singleton game startup are not solved by these changes. The write backlog cap is not a global memory bound.
5. Hardware/live Android/WoW, compositor, Docker and user-device tests are prohibited in this task and remain unverified. Full compile and isolated runtime results are listed below; they do not substitute for those acceptance checks or independent review.

## Checks

- AST MCP with the explicit worktree root reported no index; no rebuild was started. Used focused ranges/diff. No `tools/agents/manifest.json` exists here.
- Build root: `/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6`, source worktree remains in the approved Worktrees root. The sandbox initially presented Games read-only; host Manager verified rw and an authorized `require_escalated` write check passed. No remount/system install/fallback build root was used.
- Isolated real production `moonlight.cpp`, `crypto/*.cpp`, `tests/testCrypto.cpp`, `tests/testControl.cpp`, Catch2 3.8.0, clang++ C++20, `-fsanitize=address,undefined -fno-omit-frame-pointer`, `-j2`: **17 cases, 2048 assertions PASS**. Exact DER pin, foreign/self/same-key/reissued/expired/null certs; legacy captures; V2 directional vectors at 0/255/256/UINT32_MAX; wrap refusal/key reuse/capacity/replay; every input subtype truncated and internally re-framed; controller boundaries; binary tags and malformed envelope/UTF lengths.
- Additional authored `testWolfAPI.cpp` regressions: real socket slow-reader SSE with two producers and two io runners; exact frame integrity; wrong/ambiguous token, explicit legacy IP path and new generation rejection. **2 cases, 145 assertions PASS with ASan/UBSan** in the isolated API runner. No Wolf startup or game subsystems were used.
- No exact token usage is available; no invented `report_usage` was submitted. Manager owns tracker leases.

### Build qualification (environment-only adjustment)

Vanilla full configure stopped because the fetched Simple-Web-Server performs a second `find_package(Boost)` despite Wolf already supplying Boost through FetchContent. Manager approved a local dependency-CMake adapter: skip that lookup only when the real `Boost::boost` target exists, otherwise keep the normal lookup. Enabled Boost's own `BOOST_ENABLE_COMPATIBILITY_TARGETS=ON`. No runtime implementation, project CMake or system package was changed to bypass this issue.

Dependency SHA: `546895a93a29062bb178367b46c7afb72da9881e`. Adapter patch SHA256: `f535fdee2fdb171a8957703ec6fa5e0c3bc0ee605baf2c02d8213908cfbfd12c`. [Exact patch](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/simplewebserver-cmake.patch), [provenance](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/simplewebserver-cmake-provenance.txt), [adjusted configure log](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/full-configure-adjusted.log). This qualifies compilation with a documented build-environment adapter, **not** a vanilla upstream reproducible full build.

The external [API test target](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/api-security-target.cmake) links the real `wolf::runner` and `tests/testWolfAPI.cpp` with Catch2's main, bypassing Wolf's test startup (`streaming::init`, device/environment discovery). It adds ASan/UBSan to the runner/API test and never substitutes production implementations. Full executables are compiled only; the allowed isolated selector is `[security]` on that separate runner.


The fetched Boost subset also omitted `property_tree` headers used by the unchanged protocol. The external target file supplies the official monolithic **Boost 1.87.0** include tree (the same version as the compiled Boost libraries). This additional environment qualification is explicit; no fake headers or implementation stubs were used.

`CMAKE_CATCH_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST` is mandatory for build-only checks: the upstream `wolftests` main initializes streaming even for `--list-tests`. The initial build was intentionally interrupted during dependency compilation, before any discovery, then resumed with PRE_TEST. Neither the `wolf` executable nor upstream `wolftests` main was run.


### Final verification result

- Adjusted full Debug/shared build of **wolf**, **wolftests** and **mwow-api-security**, clang++ C++20, `-j2`: **PASS**, including a final incremental pass after the last production header change. [Build log](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/full-build.log), [final incremental log](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/final-incremental-build.log).
- Real production crypto/control test runner: **17 cases / 2048 assertions PASS**, ASan/UBSan. [Log](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/isolated-tests.log).
- Real production API/association test runner `[security]`: **2 cases / 145 assertions PASS**, ASan/UBSan, including exact SSE frames beyond socket buffering with concurrent publishers/io runners. [Log](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/api-security-tests.log).
- `git diff --check` and targeted `clang-format --dry-run --Werror` (including the adapter): **PASS**.
- [Final configure/build/test argv and tool versions](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/build-commands.json). The JSON includes hashes of both external build adapters. The isolated minimal build recipe is [CMakeLists.txt](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/isolated/CMakeLists.txt).
- Existing unrelated warnings remain visible in logs, including ignored `gst_buffer_append` return value in `gst-plugin/utils.hpp:75`. No warnings were hidden to obtain a pass.
- This checks the shared-library Debug build on this Linux host. GCC/static/Release matrices, TSan, complete upstream tests, actual ENet/reconnect/network behavior, hardware/live Android and MWOW-9 lifecycle/barrier acceptance remain outstanding. No deployment, merge, push or release.

Source is frozen at the reported security commit for independent review; do not treat these passes as user acceptance. The approved worktree/build environment is retained for Manager's combined candidate with MWOW-7.

## Independent review corrections F3–F5 (after 8395abc)

Status: **review candidate**, not accepted. These changes are a separate commit on
`feature/MWOW-6-security-fix`, based on `8395abc30592640b9f89091cea3be5f4e7212554`;
MWOW-7 lifecycle changes are not included. The prior verification section describes
8395abc; the evidence below supersedes its input-validation coverage.

- **F3:** controller-touch wire pointer IDs are checked as unsigned 32-bit values
  before conversion to inputtino's signed finger index. The existing direct mapping
  supports only PS5 touchpad slots 0 and 1. IDs 2, INT_MAX, 0x80000000 and UINT_MAX
  fail before any device operation, for every event type. Screen-touch IDs retain
  their separate semantics and are not restricted to two slots. CANCEL_ALL and
  BUTTON_ONLY remain unsupported/no-op for controller touch; this patch does not
  implement multi-touch ID remapping or MWOW-9 neutralisation.
- **F4:** battery states must be 0..5; percentage must be 0..100 or 0xff (unknown).
  Unknown/not-present reports and unknown percentage make no device call. The typed
  device dispatch validates independently of the outer packet and has a safe default
  switch branch, so invalid enums cannot read an uninitialised battery state.
- **F5:** every netfloat in controller touch/motion, screen touch and pen is checked
  for finiteness before dispatch/device creation. Finite normalized coordinates and
  pressure preserve the existing 0..1 clamping behaviour. Contact-area fields are
  currently unused but nonfinite values are rejected too. Gyroscope degree/radian
  conversion uses double arithmetic. Finite extreme motion is saturated in the
  actual patched inputtino encoder before integer conversion; negative motion is
  preserved. Motion/event/tool enum checks reject unsupported values.

`control/input_dispatch.hpp` contains the small production PS5 device-boundary
functions used by `input_handler.cpp` and by recording-sink tests. Tests do not copy
those implementations or instantiate devices. Typed touch/pen handlers also apply
field validation before their lazy device creation. Existing V2 and legacy capture
vectors are unchanged.

### Tracked inputtino correction and source ownership

Review of the pinned dependency also found that `htole16(float)` converted negative
samples directly to an unsigned integer. The tracked
[patch](../../patches/inputtino-signed-motion.patch) replaces that conversion with a
private `motion_encoding.hpp` helper used by the real `PS5Joypad::set_motion`.
It scales in double, clamps to the signed 16-bit range, converts to `int16_t`, then
uses the defined signed-to-unsigned modulo conversion before endian encoding.
Nonfinite samples at this dependency boundary encode as zero; Wolf rejects them
before reaching the dependency. No dependency API, physical device code, sensor
units or wire format was invented or replaced with a test copy.

The pin remains **fd136cfe492b4375b4507718bcca1f044588fc6f**. `FetchContent`
automatically applies [the checked driver](../../cmake/PatchInputtinoMotion.cmake)
to the fetched source, with exact before/after SHA256 checks. Repeated application
is a no-op only when both patched source and helper hashes match. Unknown source
or a conflicting helper fails closed. Both `WOLF_CUSTOM_INPUTTINO_SRC` and
`FETCHCONTENT_SOURCE_DIR_INPUTTINO` are **verification-only** paths: an unpatched
custom tree fails configure with instructions; it is never silently modified.
Custom inputtino users must explicitly apply this patch to the matching pin or
review a new compatible integration when upgrading.

Actual build source: `MWOW-6/full-build/_deps/inputtino-src`, fetched afresh by the
normal CMake path. The old `MWOW-6/inputtino-fd136cf` override remains unchanged.

| Artifact | SHA256 |
|---|---|
| Original `src/uhid/joypad_ps5.cpp` | `f4a5a170e7a39ac63ba4f61fd73d233bdb2d536ec9fa9736f8e9b904e376987e` |
| Patched `src/uhid/joypad_ps5.cpp` | `4a29d8b05f58c07f06366ae6d91e0d19e058f9d20710e3d20d45ae3ee8e800d1` |
| New private `motion_encoding.hpp` | `342fe83ced8fcb7bc964757b631b86622458511aeb5e9b066708c5586034623c` |
| Tracked patch | `5b852c8c092b2e29f2d2c29753bce0d7580d5f443568032b94132c72d49d934b` |

`tests/CMakeLists.txt` registers **wolf-inputtino-motion-tests** with Catch2's own
main, including the actual patched dependency helper. It requires no devices and
can be executed directly without entering Wolf's test main. This is tracked CI
registration, not merely an external test recipe.

### Correction evidence and remaining limits

Final corrected-source checks: **21 crypto/control cases / 7965 assertions**, **1
actual inputtino-helper case / 35 assertions**, **2 API/SSE cases / 145 assertions**
all PASS with UBSan halt-on-error. Adjusted Debug/shared production build targets
`wolf`, `wolftests`, `mwow-api-security`, `wolf-inputtino-motion-tests` PASS at `-j2`
with PRE_TEST. Changed C++ formatting and `git diff --check` PASS. Existing
range-v3 deprecation, crypto VLA-extension and ignored GStreamer return warnings
remain in the raw logs; no suppression was added.

The exact final correction revision, argv, exit codes, source hashes, dependency
hashes and raw log hashes are recorded in
[review-fixes-verification.json](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/review-fixes-verification.json).
Configure uses [these arguments](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/review-fixes-configure-argv.json),
with the previous custom inputtino source override removed. The remaining
SimpleWebServer/Boost build qualifications above still apply; this is not a vanilla
upstream build. The external instrumentation file additionally instruments the
real inputtino library and its device-free test with ASan/UBSan/float-cast-overflow.

Tests cover all 256 battery enum byte values and percentage boundaries; all eight
controller-touch event kinds with boundary IDs; each netfloat field with NaN,
+Inf and -Inf; FLT_MAX, lowest finite float, minimum normal and denormal samples;
negative signed-motion wire vectors and saturation/truncation boundaries. Invalid
PS5 inputs must produce zero recording-sink calls. Private dependency-helper tests
exercise the implementation compiled into the real library, not a rewritten formula.

Idempotent patch application, verification of a patched override, and rejection of
an unpatched override without modification are captured in
[patch checks](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/review-fixes-patch-checks.json).

No Wolf main, upstream wolftests main, live API/game, virtual input devices or Docker
workloads were run. This correction does not claim RCE exploitation, hardware
neutral delivery, an ENet handoff barrier, full lifecycle cleanup or acceptance.
MWOW-9 remains deferred; its read-only design draft is preserved in thread storage.
