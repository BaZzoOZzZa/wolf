# MWOW-10 — tracked CMake dependency fixes

Status: **review candidate; clean full configure pending Manager's heavy-slot grant**.
Base: `0e3710be36a6a7fa130f633b358d1b520a3cee9c`, branch `feature/MWOW-10-build-fix`.
No merge, push, system installation, Wolf/test-main startup, game, Docker or device operation.

## Changes and provenance

- Boost's FetchContent fallback enables its own `BOOST_ENABLE_COMPATIBILITY_TARGETS`.
  This produces the real `Boost::boost` → `Boost::headers` compatibility chain.
  It selects `property_tree` as an additional header-only source component and
  appends **`Boost::property_tree`** to `Boost_LIBRARIES`. The existing PUBLIC
  `${Boost_LIBRARIES}` link on `moonlight` therefore supplies its public
  `moonlight/protocol.hpp` with property_tree and its transitive dependencies.
  The same list reaches `wolf_runner`. No synthetic target, stub, host include
  or monolithic Boost overlay is introduced. The old fallback-only
  `libs/headers/include` global include is cleared: it is not the aggregate
  include tree in Boost's split source distribution.
- The existing installed-Boost lookup (minimum 1.87.0 and original compiled
  components) is unchanged. Header-only `property_tree` is selected explicitly
  only in the source fallback, avoiding a new required installed component
  package on systems whose Boost packaging exposes the common headers instead.
  The fetched version remains **1.87.0** from the existing official release URL.
  Its real `libs/property_tree/CMakeLists.txt` defines `Boost::property_tree`
  and its transitive target links. No independent header version is mixed in.
- Simple-Web-Server is pinned to the full previously qualified commit
  [`546895a93a29062bb178367b46c7afb72da9881e`](https://gitlab.com/ReenigneArcher/Simple-Web-Server/-/tree/546895a93a29062bb178367b46c7afb72da9881e).
  FetchContent runs the tracked `cmake/PatchSimpleWebBoost.cmake` automatically.
  It skips the redundant Boost lookup only when `Boost::boost` already exists;
  otherwise it preserves both original lookup attempts and their requirements.
  The operation is idempotent and rejects unknown upstream content. Its output
  is byte-identical to the qualified MWOW-6 CMake adapter. Cached dependencies
  need no manual edits, and no external `CMAKE_PROJECT_INCLUDE` is needed for
  these fixes. Explicit `FETCHCONTENT_SOURCE_DIR_SIMPLEWEBSERVER` overrides
  bypass the normal download/patch step in CMake; do not use one to qualify this fix.
- `mdns_cpp` is pinned to
  [`0d57ae3db73d94ab2e7ac0a799a9165c8a221c80`](https://github.com/games-on-whales/mdns_cpp/commit/0d57ae3db73d94ab2e7ac0a799a9165c8a221c80),
  obtained using `git rev-parse HEAD` on the actual MWOW-6 qualified dependency.
  Its commit subject is `fix: allow control over running it in a separate thread`
  (2024-10-19). No new upstream HEAD was substituted.
- `docker/gstreamer.Dockerfile` is unchanged. The supplied qualified-build
  evidence does not identify an actually qualified gst-interpipe SHA; choosing
  an arbitrary current HEAD would not establish that provenance. Its floating
  clone remains a reproducibility residual, as do other unpinned build inputs.

Historical security report read with `git show`, not from the moving physical
MWOW-6 source: `8395abc30592640b9f89091cea3be5f4e7212554:docs/mwow/MWOW-6-security.md`.
Evidence root: `/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-6/`:
`build-commands.json`, `simplewebserver-cmake.patch`, `api-security-target.cmake`,
`full-configure-adjusted.log`. That qualification used external adapters; its
full-build success is **not** a clean-configure result for this revision.

## Checks completed

Host verified as `host_bgvkpihs3j`; source canonical path is inside
`/run/media/bazzoozza/work/Worktrees` on mounted ext4. No new worktree was created.
No `tools/agents/manifest.json` exists. AST MCP with the explicit source root
reported `Index not found`; focused file reads were used without a rebuild.

Using CMake **4.4.4**, Git **2.56.0**:

- `git diff --check`: PASS.
- `cmake -DSIMPLEWEBSERVER_SOURCE_DIR=<fixture> -P cmake/PatchSimpleWebBoost.cmake`:
  PASS on pristine pinned source; repeat leaves identical bytes.
- Result equals the exact MWOW-6 qualified SimpleWeb CMake file: PASS.
- Unsupported upstream content: rejected with exit 1 and the intended diagnostic.
- Pinned mDNS SHA equals the qualified dependency HEAD: PASS.
- Official fetched Boost version is 1.87, and its property_tree target declares
  real transitive dependencies: static source inspection PASS.

Exact script-test argv, results and script SHA256:
[`static-checks/results.json`](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-10/static-checks/results.json).
These are script/static checks, not generated target-property, compile or runtime tests.
MWOW-6 caches/sources were read only; fixtures were written under MWOW-10.

## Pending clean qualification

**Do not run until Manager grants the single heavy slot.** Use the approved
build root and a fresh, unused binary directory; no cached dependency source
overrides, external project include, compatibility-target CLI switch or Boost
header overlay. Proposed exact argv (not executed):

```bash
cmake -S /run/media/bazzoozza/work/Worktrees/mirror-wow-wolf-build-fix \
  -B /run/media/bazzoozza/Games/bb-targets/wolf/MWOW-10/clean-configure \
  -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_STANDARD=20 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DADD_STACKTRACE_INFO=OFF -DTEST_RUST_WAYLAND=OFF -DTEST_DOCKER=OFF \
  -DTEST_VIRTUAL_INPUT=OFF -DTEST_UHID=OFF -DTEST_NVIDIA=OFF \
  -DTEST_EXCEPTIONS=OFF -DCMAKE_CATCH_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST
```

Save the full configure log, cache and actual resolved dependency revisions.
Verify generated `moonlight` include paths resolve property_tree from that
Boost 1.87 source and its PUBLIC dependency contains `Boost::property_tree`.
Also qualify the installed-Boost branch with a real installed package; no
synthetic package/targets should stand in for that check. Source and installed
paths, shared/static and GCC/Clang remain unexecuted on this revision.

`PRE_TEST` is intentional: upstream `wolftests` main initializes streaming even
for discovery. No build/discovery/test execution is authorized in this step.
Inputtino motion fixes and tests owned by MWOW-6 are outside this diff and may
be needed for later compilation. No OOM fix, complete BOM, runtime compatibility,
full clean-build success or user acceptance is claimed. Exact usage is unavailable;
Manager owns the Jiraia lease and no usage numbers are invented.
