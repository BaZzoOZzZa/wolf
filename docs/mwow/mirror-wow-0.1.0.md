# Mirror WoW 0.1.0 — silent MVP

MWOW-9: first MVP accepted by the user on the existing host, with WoW image and
touch input working on iPad through Moonlight. Audio is intentionally silent.
This is a source integration release, with no new integrated binary build.

## Source and runtime provenance

The [manifest](mirror-wow-0.1.0.manifest.json) records full source heads and
owner-supplied deployed hashes. The native, Rust controller and image branches
were merged in full, in that order, using three `--no-ff` merges into `develop`.
The release branch adds this runbook, the manifest and the operational helper.
Native ancestors, including historical MWOW-10 build documentation, belong to
the exact deployed native head; no separate MWOW-10 branch is integrated.

Deployed executable, crypto and runner hashes describe the accepted runtime's
separate artifacts. They do not prove that an integrated release binary was
built. Host-dependent shared objects are loaded through RUNPATH. Preserve these
dependencies with the accepted deployment; do not rebuild over live build trees
or overwrite mapped files. The guest image ID identifies the local immutable
image, and does not imply a publicly available registry image or portable bundle.

## Operator runbook for the existing accepted deployment

1. Use the already approved host provisioning, private profile/spec and existing
   Moonlight session. The checked-in example profiles contain placeholders and
   are not deployment configuration. Background on the controller and its
   fail-closed `Unknown` state is in [Rust isolation](MWOW9-rust-isolation.md);
   image provenance is in [isolated image](MWOW9-isolated-image.md). Those files
   retain historical source checkpoints; this release note describes the final
   accepted MVP boundary.
2. Before direct game launch, keep exactly one existing `WolfWowIsolated_` guest
   running from the exact image ID in the manifest. Its Battle.net/Agent Wine
   session must already exist. The helper does not create a container, provision
   Wine, authenticate an account or repair an epoch.
3. From this source checkout on the accepted host, explicitly request DX11:

   ```sh
   python3 tools/wolf-isolated/operations/launch-wow.py --dx11
   ```

   This helper is byte-identical to the runtime-verified operational script.
   It checks for an existing WoW instance on the host and in the selected guest,
   requires one matching approved-image guest, and borrows the existing Wine
   session context. It launches `/games/wow/_forever_/Wow.exe -d3d11` as UID 2001
   with child-only `WINEDLLOVERRIDES=dxgi,d3d11=n,b`. The default invocation
   without `--dx11` does not append `-d3d11`; use the command above to reproduce
   the accepted route. These guards are an operator aid, not an atomic global
   singleton guarantee under concurrent invocations.
4. Authenticate manually in the game if needed. Direct launch does not transfer
   a Battle.net authentication ticket; the normal Battle.net route remains a
   separate option. The default Battle.net guest does not inherit this helper's
   DXVK override. Verify image and touch on the connected iPad; expect silence.
5. If closing/resuming Moonlight leaves a retained Docker name or controller
   `Unknown`, stop the retry loop. Preserve the private journal and ownership
   evidence for the operator's guarded archive/fresh-epoch procedure. This
   release supplies no automatic cleanup command: ownership must be resolved
   before another epoch. Reconnect is not fixed by this release.

The helper writes operational output inside the guest under `/tmp`; those logs,
private profiles, credentials, receipts, game bytes, Wine prefixes and screenshots
are not release source or public artifacts. Do not commit them.

## Qualification and limitations

- Battle.net updater reported `CascLocked` while WoW was open. SSL libraries and
  CA data were present; an SSL root cause has not been established.
- Silent mode is the only supported MVP audio mode. There is no real audio
  capture PASS, no qualification on another machine and no production-grade
  security or performance claim.
- User physical acceptance applies to the current host's image/touch MVP; it
  does not complete every security, lifecycle or performance gate.
- Source integration checks cover merge ancestry, unchanged deployed blobs,
  Python syntax without execution, whitespace and a clean checkout. Tests,
  clippy, formal review and broad/heavy builds were waived for this integration.
  No new binary build or runtime test is claimed.

Publication, artifact packaging, tags and subsequent main-branch integration
remain separate release-owner actions. This integration performs none of them.
