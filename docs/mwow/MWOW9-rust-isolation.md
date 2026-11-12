# MWOW-9: Rust isolation launcher — foundation, not runtime

## Implemented boundary

`tools/wolf-isolated` is a standalone, offline-buildable Rust CLI. Only `inspect PROFILE.json`
and `plan PROFILE.json` are implemented. They read bounded operator-owned JSON, inspect file/directory
metadata, hash bounded regular artifacts, and emit schema-1 JSON. Exit 0 means file observations
matched the declaration; **never** that isolation, permissions, image contents or runtime are ready.
Invalid/missing observations exit 2. `start`, `status`, `stop` exit 3 with
`runtime_stage_not_implemented`, without starting/signalling anything. There is no daemon client,
shell execution, user creation, udev installation or game/prefix mutation.

Limits: profile 64 KiB before JSON parse; six unique artifact roles, each <=256 MiB streamed;
paths <=4096 bytes, exact render + event node, fixed mount roles, no arbitrary command/mount list.
JSON-size limits bound owned strings/arrays; these are not whole-process RSS guarantees. Host OS,
architecture and optional boot ID are diagnostic facts, not an authority token. Metadata and hashes
are read-only observations, not an atomic start permit. Operator-owned paths must remain stable:
this foundation does not defend against a concurrent privileged path-replacement adversary.
It rejects symlink observations but does not implement future race-free fd-relative runtime pinning.
No device node is opened by the inspector; node checks use metadata only.

Example `profiles/wow.example.json` is deliberately UNPROVISIONED: UID/path/digest/node placeholders
are not discovered facts. Manager must supply the actual runner/prefix identities. No presumed
`~/.wine`, account tokens, environment dump, or authentication copy guarantee.

## Reproduce software checks (no runtime)

From this new worktree, with Games mounted and the host CPU slot coordinated:

```sh
CARGO_TARGET_DIR=/run/media/bazzoozza/Games/bb-targets/wolf/rust-isolation \
  cargo test --offline --locked --manifest-path tools/wolf-isolated/Cargo.toml -j2
```

The resulting `debug/wolf-isolated plan tools/wolf-isolated/profiles/wow.example.json`
emits a proposal plus missing-observation errors (expected exit 2), not a launch.
No runtime command accepts arbitrary executable arguments.

## Concrete topology and security boundary

One remote UID owns native Wolf's private `waylanddisplaysrc` compositor and virtual Xbox;
Gamescope (Wayland backend) + its Xwayland + Wine/Battle.net run in a restricted guest.
Minecraft remains on the existing desktop UID/display. No host X11 socket, DISPLAY, desktop
runtime/home, whole input directory, Docker socket or physical mouse/keyboard reaches the guest.
Only the exact current remote Xbox event node and render node are assigned; declared input/render GIDs become explicit guest supplemental groups, with node group/mode checked as metadata. Keyboard/mouse are
routed to the private compositor, not the e1 desktop XTEST backend. e1 remains independently usable.

This prevents **automatic** input/focus crossover if the remote device nodes have a private group,
no desktop ACL, and no already-open/inherited desktop FD. Same UID/display alone is insufficient.
A new LOCAL gaming UID/login, logout, or removing the current user's docker membership is not
required for this threat model. Root/parent/docker administrators remain trusted and can deliberately
bypass it. GPU sharing is neither performance isolation nor protection against driver defects.

Plan emits argument arrays, not a runnable shell script. `docker create --pull=never --cap-drop=ALL
--security-opt=no-new-privileges --network=wolf-wow-egress --ipc=private --read-only --user=REMOTE:REMOTE`
is the intended guest boundary, with a private `/dev`, no MKNOD, exact node `rw` permissions, tmpfs
`/tmp`, working prefix RW, separate game tree RO, and exact Wayland/Pulse sockets. No `--privileged`,
no host namespaces, no wildcard input majors. `wolf-wow-egress` is an **unprovisioned** dedicated
bridge with reviewed outbound policy, not a claim of working Battle.net connectivity; the current
CLI never creates/inspects it. The fixed guest entrypoint must clear inherited environment/FDs,
start Gamescope, and execute the approved Wine runner. Entrypoint, image layout, dependencies and
socket/node ownership must be verified before implementing start. Plan's executable locations are
an image build contract, not evidence the image already contains them. No generic arbitrary exec.

## Precise provisioning proposal — NOT installed

1. Allocate unused non-root remote UID and private groups `wolf-wow-input`, `wolf-wow-uinput`.
   Only remote service joins them; desktop UID does not. Remote home/prefix/runtime owned by that
   UID with 0700 ancestors. Runtime must be established using a reviewed service/logind mechanism;
   merely creating `/run/user/UID` is not a login/session proof. Launcher daemon access remains
   separate trusted lifecycle authority, never passed into Wine. No local desktop account changes.
2. Review `provisioning/72-wolf-wow.rules.in`: exact actual name/VID/PID from
   `src/moonlight-server/control/input_handler.cpp:109–112`, inputtino
   `fd136cfe492b4375b4507718bcca1f044588fc6f` (develop CMake currently abbreviates `fd136cf`).
   Only that Xbox event/js class gets the private group/0660/seat-wolf-wow; only remote service gets
   uinput writer group. NO uhid/blanket input grant. Names identify a device class, not a session:
   initial supported topology is one remote domain; later runtime must retain exact node/devnum/
   generation and reject reused nodes. The source pin must be fully resolved/verified at artifact build.
3. Rules are an uninstalled template: proposed filename72 follows 70-uaccess tagging and precedes
   73-seat-late queuing RUN builtin uaccess. The former filename90 was too late to infer that
   queued RUN was excluded. This is not a demonstrated leak. Review actual rule order, later
   overrides and both event/js aliases before installing: TAG removal does not revoke existing
   ACLs/open FDs and other rules can override it.
   Snapshot exact prior ACL/rule state, create a fresh device generation after the approved restart,
   verify no desktop read ACL/open FD, then pass only that node. Never blanket `setfacl -b`/chmod
   `/dev/input/*`. Current temporary UID1000 uinput ACL is not silently removed by this proposal.
4. Acquire immutable guest image digest and six artifact hashes: Wolf, Gamescope, exact Wine runner,
   entrypoint, compositor plugin and interpipe plugin. Inspect/build pinned custom
   `waylanddisplaysrc` and `interpipesrc`/`interpipesink` artifacts separately; system gst-inspect
   failure is not proof of whole-host absence. Verify image contents/ELF dependencies, GPU/node,
   plugin compatibility, private Pulse/Wayland readiness. No downloads/build/provision performed here.
5. Manager supplies actual current prefix/runner paths and approves ONE clean WoW/Battle.net exit,
   backup, migration to remote-owned working prefix and one restart. Never simultaneous prefix use
   or second WoW. Copying prefix does not guarantee Bnet/keyring/DPAPI login persistence. Original
   prefix remains untouched until separate approval; game-data writable needs require review.
6. Next runtime implementation must reserve an exclusive lease before effects, pin prefix/artifact/
   node allocation identities, own child/container IDs and actual wait/join. Stop only owned exact
   identities, never broad kill. Unknown retains lease/resources, forbids replacement; timeout is
   decision only. No detach/reaper. Rollback removes only recorded installed rule/service/groups/
   owned directories and restores recorded exact ACLs after confirmed isolated-owner exit, never
   reopens the original live prefix on uncertain shutdown.

## Existing source evidence and next approval

Wolf source: `streaming/streaming.cpp:102–104` private compositor/interpipe chain;
`sessions/common.cpp:32–45` private Pulse/Wayland wiring; `runners/docker.cpp:149–173`
current broad dynamic device-major rules (NOT sufficient by themselves); `runners/process.cpp`
host execution is NOT an isolation boundary. This crate does not call/change any of them.

Request separately: review this foundation, approve exact account/group/rule/runtime/network/image
provisioning plan and artifact acquisition/build, then implement owned runtime. Actual graphical/input
verification is a later explicit operation. Current user game, prefix, e1 source/build and all live
services remain untouched. Rust foundation is incomplete and cannot start an isolated session.

Checks and immutable failure/provenance manifests are in the author's thread-storage
`checks/rust-isolation-foundation`; first test compile failure is retained, not semantic RED.

## R37-1 correction (foundation only)

Game-data must be disjoint in **both** directions from desktop home, remote home and source prefix:
equal, ancestor and descendant paths are refused; distinct component siblings remain valid.
Actual validator and CLI regression tests reproduced all three ancestor proposals on RED, then use
identical test bytes on GREEN. Original tests remain unchanged. No runtime, udev, ACL or prefix
effects were added. Updated template ordering remains a proposal requiring later override review
and fresh node/ACL/FD proof before any separate installation approval.


## Owned runtime contract checkpoint — blocked before effectful code

The accepted schema-1 profile is NOT sufficient to implement an honest start. This is an
artifact/interface gap, not a request for permission to run anything. No runtime implementation
or provisioning is claimed by this checkpoint; existing inspect/plan and Unsupported commands
remain unchanged. Source-derived dependencies:

- `wolf.cpp:177–205` starts the actual monolithic server from `WOLF_CFG_FILE`/key/cert state;
  schema1 pins a binary, but has no exact reviewed isolated Wolf configuration or state binding.
- `sessions/moonlight.cpp:90–119,162–190` creates the private compositor **on StreamSession**,
  learns its real socket from the plugin, then fires StartRunner. The static plan's
  `/run/user/UID/wolf-wayland` is a proposal, NOT a produced/owned readiness receipt.
- `sessions/common.cpp:19–45,69–84` forwards actual session/environment/socket/device data into
  Runner. `runners/process.cpp:14–40` receives but does not forward the device queue/paths as a
  typed handshake; environment text is not a node-allocation proof.
- `runners/docker.cpp:181–197` itself creates/starts a guest after StartRunner. Using that runner
  AND an independent Rust create would introduce two guest owners. Rust must be the sole guest
  creator; choose a fixed process-runner bridge, not a second Docker runner or desktop fallback.

### Exact additive interface proposed for the next source grant

Keep schema1 inspect/plan and its eleven tests intact. Add a separately bounded schema-1
`RuntimeSpec` parsed only by `start PROFILE --runtime SPEC` (32KiB max, deny unknown fields):

```text
RuntimeSpec {
  profile_sha256,
  wolf_config: { canonical_path, sha256 },
  private_state_dir: { canonical_path, owner_uid, dev, ino },
  working_prefix: { dev, ino, migration_manifest_sha256 },
  image_layout_manifest: { canonical_path, sha256 },
  host_bridge: { canonical_path, sha256, protocol: 1 },
  control_directory: { canonical_path, owner_uid, dev, ino }
}
```

No `approved=true`, `ready=true`, caller-supplied PID or passive JSON can authorize effects.
The migration manifest must come from the later explicitly approved clean-stop/backup/clone
operation; it cannot be minted by inspect. Original unmanaged Wine does not honor our new lock:
flocking its path alone cannot prove it is idle. Current manual WoW remains untouched.
Image-layout manifest must bind the image digest to actual guest Wine/Gamescope/entrypoint/plugin
locations/hashes and their build provenance; the six host artifact hashes do not establish this.
The approved guest entrypoint must take the exact shared working-prefix lease before Wine;
otherwise do not start. Neither artifact/provider currently exists in this crate.

One pinned host bridge executable (inside tools/wolf-isolated, future explicit source grant) runs
as the fixed native Wolf app ProcessRunner, NOT as a generic command. A private owned Unix channel
must bind peer credentials + boot/PID/starttime + per-launch fresh nonce to the native child and
expected config/app. It transfers a bounded typed `SessionReadyV1` with actual source session,
Wayland/Pulse socket identities and fresh Xbox node devnum/inode/generation. Raw inherited env,
name/VID/PID or current static event path alone cannot seal this record. Resolve how the bridge
obtains exact initial/hotplug device metadata from the actual Runner queue before implementation;
if the existing process runner cannot expose it safely, request a narrow C++ adapter separately.
No broad protected-stage work is required, but no fabricated readiness receipt is acceptable.

### Minimal lifecycle algorithm after that bridge contract is reviewed

Reserve a private process/domain AND migrated-prefix journal/OS lock before child/engine effects.
Journal contains profile/spec digest, launch nonce, boot ID, owner PID/starttime and exact child/
container identities; no keys/account/environment. Run foreground in an explicitly provisioned
remote-UID service context; do not silently setuid/create a user. One owner, no background daemon.
Verify all prerequisites first, start the fixed native Wolf child with explicit clean environment,
then wait for real bridge readiness (AwaitingSession is not game-ready). Only this owner creates
one guest with the fixed inspected mount/cap/device arguments. Never invoke Wolf's Docker runner
as a second owner. Each effect is preceded by its journal obligation; uncertain create retains
nonce/label and Unknown even if no CID was returned. Recover only by exact nonce+profile identity,
never by a generic container name/global PID scan.

Status/stop require the exact current private journal/boot/owner identity. Status absence is not
Stopped. Stop requests exact owned container/child only, then requires actual engine inspect-exit
AND child wait/join (including bridge shutdown) before lease release. Deadline/daemon loss/child
exit alone => Unknown, retained journal/lease, no replacement/remove/broad kill. No callback/IO or
last arbitrary capture destruction under the bookkeeping gate. Recovery/reconciliation is explicit,
not successful cleanup inferred from CLI exit. Cross-boot journals require reconciliation, not PID
reuse. No Minecraft identity is a permissible stop target.

### Next bounded implementation and actual test matrix

After approving the additive spec/bridge producer contract, own only existing
`tools/wolf-isolated/{src,tests,profiles}/**` + this doc: one `runtime.rs` controller, one small
journal helper and fixed bridge mode; no generic executor/framework. Inject primitive engine,
process, clock and journal IO in tests, while production controller performs all identity/state
checks. Cover busy lease before effects; missing spec/plugin/image-layout/migration/device proof;
exact partial-create rollback; uncertain create/start/inspect/child exit retains Unknown; exact
verified exits release; foreign/stale/boot/PID reuse stop denial; concurrent reserve refusal;
Minecraft/unowned IDs untouched; cleanup reentry outside gates. Pure CLI refusal tests must assert
zero effect calls, not accept a fake success bool. Existing foundation tests remain byte-identical.

**Current checkpoint limitation:** no controller/bridge/attestation producer was invented to make
mocks pass. Source runtime and its new tests are blocked on the above concrete artifact handshake;
no Cargo rerun is warranted for this doc-only delta. Request a narrow next source grant for the
spec+bridge producer boundary before implementing effectful lifecycle. Provisioning and physical
start remain separate later approvals. No users/rules/ACLs/prefix/graphics/network/game changes.


## Architecture decision: reuse reviewed native DockerRunner (supersedes bridge proposal)

**Decision: choose native guest ownership + small Rust outer supervisor.** The preceding
Rust-sole-guest-creator / SessionReady bridge section is HISTORICAL and superseded. It must not
be used to require a new readiness/IPC/attestation subsystem. The schema1 docker-create argv is
also historical proposal output, never an executable path for the new supervisor. No code or
provisioning is enabled by this doc.

Use the reviewed e1 native artifact, NOT the C++ develop source in this Rust worktree:
`e1fdfd2e12b79bfd30e6a776478ead29ce958e67` in `mirror-wow-wolf-handoff-fix`.
Its RunDocker differs by 80 additions/38 deletions from develop0e3710b; fake-udev source is identical.
Actual dynamic compositor/Pulse paths, scoped RunScope/device queue, guest create/start and
ContainerSupervision remain native-owned. Rust reserves outer domain/prefix lease, starts/stops
only its exact native Wolf child, and observes guest exit using read-only Docker inspection.
No second create/start/stop/remove controller, guessed socket/device, or producer bridge.

### Fixed config boundary sufficient for the intended supervised threat model

- One app/allowed client, real virtual compositor + private Gamescope/Xwayland; no desktop
  XTEST/X11 socket/runtime/home or whole host /dev/input bind. Private guest /dev and IPC/network
  namespaces; only render devices plus Wolf-created scoped virtual devices/fake-udev metadata.
  All automatically appended native mounts/devices must be validated too, not just TOML literals.
- Remote host UID/private udev group/seat, no desktop input ACL or inherited/open FD, still require
  separate provisioning/fresh-node proof. Minecraft keeps the desktop UID/physical devices.
  Administrators/root/docker remain trusted. Shared GPU performance is not isolated.
- The earlier all-container-CapDrop-ALL plan is NOT usable unchanged: e1 docker.cpp302–310 executes
  `mknod ... && chmod ... && fake-udev` as root. MKNOD must remain available to that trusted helper;
  CapDrop ALL alone makes pad hotplug fail. Source configuration.adoc230 documents the helper
  candidate allowlist NET_RAW/MKNOD/NET_ADMIN; fake-udev uses netlink bind/sendmsg (hpp17–47).
  This does not prove all three are minimal or sufficient on the deployed kernel/seccomp profile.
  Do not add SYS_ADMIN/privileged/host IPC as a shortcut; validate the helper separately later.
- Wine must run as remote non-root UID with effective/permitted/inheritable/ambient caps cleared,
  no-new-privileges, no setuid/file-cap escalation path, no daemon socket. A fixed reviewed image
  entrypoint enforces this per Wine process. Container-level helper capability allowance is NOT
  Wine permission. Do not assume PUID env or `User` alone proves the process capability state.
  Such image/entrypoint evidence and later actual process inspection are still missing, not a
  boolean readiness flag. No blanket chmod of host nodes: native chmod777 applies to newly mknod
  nodes inside the private guest /dev. Native broad input/hidraw cgroup majors are not by themselves
  host-node visibility; trust only scoped helper events + absent physical nodes + capless Wine.

### No new native adapter needed for the conservative first supervisor

`core/.../docker.cpp:250–264` preserves custom create JSON fields, including Labels. Fixed per-launch
nonce/profile-digest labels in the single app's base_create_json identify the native-created domain;
Rust reads exact returned CID/labels/image/config, never adopts by name/PID/title. Keep
`WOLF_STOP_CONTAINER_ON_EXIT=FALSE` so an exited guest remains inspectable rather than disappearing
before the observer sees its ID. Native runner remains the sole guest mutator and handles root
helper/device queue. Native create conflict/Unknown is not permission to replace.

Rust stop requests SIGINT only through its retained native child handle, not an arbitrary PID or
Unix numeric-session endpoint. e1 wolf.cpp232–264 sends captured GameStopRequest scopes and joins
control, but explicitly does not prove completion of all detached subsystems. Thus native exit0,
log text or a one-time empty label list MUST NOT release the lease. Release only after exact native
child wait AND positively observed exact guest exit with a conclusive native-creation history.
Missing CID, ambiguous/delayed create, daemon loss, unexpected extra labeled guests or incomplete
history => Unknown + retained journal/lease, no replacement. In particular a POST accepted before
child exit may finish later: one empty enumeration cannot prove no pending create. This conservative
Unknown outcome satisfies the current scope; do not invent a completion ACK to avoid it. A later
small completion ledger/observer adapter is optional only if automatic recovery from that Unknown
is actually required, not a prerequisite for first usable foreground supervision.

### Exact smallest next source scope

Only tools/wolf-isolated/** + this doc, no e1 edits: add one outer runtime controller and a small
Linux child/journal adapter; fixed config validation binding reviewed e1 binary/image/entrypoint,
per-launch labels, config/prefix paths and private state. Inspect known image locally before spawn;
refuse missing image (native API can pull on404, so no guarantee against an administrator removing
it concurrently). Never use this worktree's develop Wolf binary. No arbitrary exec/config passthrough.
Foreground owner reserves exclusive domain+working-prefix lease before child spawn, stores boot/
PID/starttime/nonce/config digest, uses exact owned child wait and bounded read-only engine
observations. Stop has no guest-mutation fallback; timeout => Unknown. No original-prefix flock
claim: clean stop/backup/migration to the separately leased working prefix still needs approval.

Software tests: busy lease/no spawn; malformed config/forbidden mounts/no effects; helper versus
Wine-capability policy distinguished; child spawn failure; same CID exit proof versus daemon/
create-history Unknown; foreign/ABA stop refusal; native exit alone never Stopped; no Docker mutation
operation in outer adapter; concurrent launch blocked; unowned Minecraft identities untouched.
Existing eleven foundation cases remain intact. Native/helper/image/physical checks are future
explicit grants, not part of this read-only decision. No build/runtime/device/game/system changes.


## Authorized outer-runtime implementation contract

Implement a single-thread foreground owner, no daemon or guest mutation client. `start PROFILE SPEC`
checks root-owned immutable profile/spec/config/image/prefix contracts before reserving private
journal + prefix lease. One typed controller calls injected primitive IO for reserve, spawn,
read-only observation, pidfd stop, child wait and release; no callbacks under a controller lock
(the controller has no shared lock). `status/stop PROFILE SPEC` use the private journal/control
socket exact boot/PID/starttime/nonce identity, never arbitrary PID signalling. Unknown is sticky,
retains lease/journal and never auto-releases. Positive release is limited to the declared single-
client native topology: one previously observed running labeled exact CID, no observation failure/
identity change/multiple guests, final exact guest exit + actual child wait. No CID or late/unobserved
create remains Unknown. Root/docker admin intentionally changing that topology is outside scope.
Wine caps/entrypoint and prefix migration are provisioned root-owned contracts, not inspect success.
No production provisioning records exist here; examples must fail before effects. Root contracts
must pin reviewed e1 binary revision and digests. Real TOML v7 + nested Docker JSON are parsed with
cached pinned toml0.8.23. Runtime code is source only until review + explicit physical start grant.

## Статус внешнего контроллера (источник, не приёмка)

Rust-контроллер (`start|status|stop PROFILE SPEC`) реализован и проверен только юнит-тестами с
инжектированными примитивами (offline, `-j2`): реальные Docker/X/устройства/Wolf не запускались.
Контейнеры игры создаёт и останавливает только нативный reviewed e1 RunDocker; Rust лишь читает
Docker (GET) и владеет lease/journal. Мёртвый владелец: `status`/`stop` отдают только вердикт
журнала (`Stopped`/`Refused` при том же boot ID, иначе `Unknown`), без сигналов. Inspect отвергает
privileged/host-namespace и mounts docker.sock/X11/`/dev/input`/udev. Неверифицировано: совместимость
Pulse `unix:` пути с нативным mount, образ/entrypoint/кэпы, миграция префикса. Provisioning, запуск
и приёмка изоляции — отдельные решения Manager/пользователя.
