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
2. Review `provisioning/90-wolf-wow.rules.in`: exact actual name/VID/PID from
   `src/moonlight-server/control/input_handler.cpp:109–112`, inputtino
   `fd136cfe492b4375b4507718bcca1f044588fc6f` (develop CMake currently abbreviates `fd136cf`).
   Only that Xbox event/js class gets the private group/0660/seat-wolf-wow; only remote service gets
   uinput writer group. NO uhid/blanket input grant. Names identify a device class, not a session:
   initial supported topology is one remote domain; later runtime must retain exact node/devnum/
   generation and reject reused nodes. The source pin must be fully resolved/verified at artifact build.
3. Rules are an uninstalled template. Review actual udev rule order/seat/uaccess behavior before
   installing: TAG removal does not revoke existing ACLs/open FDs and other rules can override it.
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
