# MWOW-10 — tracked CMake dependency fixes

Status: **review candidate; UUID and ENet PIC metadata checks PASS; combined build/security execution pending**.
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
  The official release archive downloaded by the zero-override run has SHA256
  `7da75f171837577a52bbf217e17f8ea576c7c246e4594d617bfde7fafd408be5`;
  FetchContent now verifies that exact archive hash with `URL_HASH`.
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

## Native isolated security infrastructure

Manager subsequently transferred ownership of `tests/CMakeLists.txt` to MWOW-10.
The existing `wolftests` source list is preserved. `WOLF_RUNTIME_TEST_DISCOVERY`
defaults to **ON**, preserving ordinary CI registration. Its discovery mode is
now explicitly **PRE_TEST**: plain compilation does not run `main.cpp` through
`--list-tests`. With the option **OFF**, no upstream Catch discovery is registered.
Do not run even `ctest -N` with that option ON in a hardware-free environment:
PRE_TEST discovery can execute the upstream main while CTest loads its test list.

Two native targets use `Catch2::Catch2WithMain`, never `tests/main.cpp`:

- `wolf-protocol-security-tests`: real `testCrypto.cpp` and `testControl.cpp`,
  linked with `wolf::moonlight`, `wolf::helpers` and the actual inputtino target
  when available. Explicit project header roots cover the control/input headers.
  It does not link `wolf::core`: that target also propagates PulseAudio .cpp
  sources to consumers. The generated protocol target contains exactly the two
  test translation units and does not acquire PulseAudio implementation sources.
- `wolf-api-security-tests`: real `testWolfAPI.cpp`, `wolf::runner`, libcurl and
  Catch2's main. Its CTest command selects **`[security]`** only. Other API cases
  require the upstream environment and must not be run through this target
  without that selector. CTest registers both isolated suites with the
  `security;isolated` labels, without executable-based discovery.

No global sanitizer flags or production substitutes were added. API security
cases and newer control headers belong to the separately frozen MWOW-6 changes;
this branch remains based on develop and does not import those changes. In this
baseline the API `[security]` selector has no cases and must not be presented as
a passing test. Actual combined compile/security execution belongs to Manager.

## Configure qualification

Manager explicitly granted MWOW-10 the single heavy slot. Full compilation,
upstream test main, test execution and installation were not performed.

1. The first fresh attempt was stopped during a slow network fmt clone. Its
   exit is `-15`; it is not a configure success. Exact command/log/exit are
   `clean-configure-command.json`, `clean-configure.log`, `clean-configure-exit.json`
   under the artifact root below.
2. A separate fresh binary directory with explicitly allowed pristine source
   cache overrides configured successfully (exit 0). SimpleWeb and mDNS used
   normal fresh FetchContent clones, with no source override. The borrowed
   Git trees were verified clean; modified MWOW-6 inputtino was not reused:
   the original pinned Git tree was archived into MWOW-10. Nothing in MWOW-6
   was edited. Exact sources/revisions/argv are recorded in
   `clean-configure-cached-command.json`; its full log is
   `clean-configure-cached.log`. This is a cache-qualified result, explicitly
   **not** the zero-override vanilla result.
3. Final cache-qualified reconfigure and target graph generation: exit 0,
   `final-configure-command.json`, `final-configure.log`, `targets.dot`.
   The real graph contains `moonlight -> boost_property_tree`,
   `simple-web-server -> boost_comptarget_boost -> boost_headers`.
   File API and compile database resolve property_tree from the split **1.87**
   source tree. No external `CMAKE_PROJECT_INCLUDE`, monolithic header include,
   synthetic Boost targets or manual SimpleWeb cache edits were used.
4. Manager then required **zero source overrides**. The initial network-only
   attempt was stopped during a slow full-history range clone (exit `-15`),
   recorded in `vanilla-command.json`, `vanilla-configure.log`, `vanilla-exit.json`.
   A Git transport-mirror attempt failed because local submodule transport was
   disabled (exit 1, `vanilla-mirrored-configure.log`). The task-local Git
   environment was then corrected and original recursive gitlinks supplied;
   another slow submodule clone was stopped (`native-off-attempt1-*`, exit `-15`).
   None of these attempts is presented as a passing configure.
5. **Final fresh one-pass configure: exit 0**, in the previously nonexistent
   `native-clean-final/` directory, 31.4 seconds configure + 0.2 seconds generate.
   Exact command/environment, complete raw log and exit/log SHA256 are
   `native-clean-final-command.json`, `native-clean-final-configure.log`,
   `native-clean-final-exit.json`. All `FETCHCONTENT_SOURCE_DIR_*` cache values
   are empty; no external project include, monolithic header overlay or manual
   dependency/cache edit is used. All dependency source trees in this binary
   directory are produced by native FetchContent clone/checkout/submodule/patch
   or official archive download/extraction.

The final run uses **explicit process-local Git transport mirrors**, rather
than downloading full Git histories repeatedly. This is a download transport
qualification, not a CMake source override: `GIT_CONFIG_COUNT/KEY_*/VALUE_*`
temporarily map declared public URLs to read-only Git repositories, with local
file transport allowed only in that process. No global Git config was changed.
Git refs/commits and recursive gitlinks are recorded in `git-mirrors.json`,
`submodule-mirrors.json` and the command JSON; needed missing refs were fetched
from their original public URLs at exact commits. Existing MWOW-6 Git objects
were borrowed read-only; their working-tree edits were never copied into the
fresh trees. SimpleWeb and mDNS were cloned directly from their declared remote
URLs. Boost was downloaded from the declared official release URL and checked
against its tracked SHA256. A full network-only download replay was not completed.

Final generated metadata/header checks: **PASS**,
[`native-final-probes.json`](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-10/native-final-probes.json):

- Native graph contains the real property_tree and Boost compatibility edges.
- `moonlight` and both security targets resolve property_tree under
  `native-clean-final/_deps/boost-src/libs/property_tree/include`; their compile
  database has no MWOW-6 or monolithic Boost overlay path.
- Actual Clang preprocessor using the generated include search path reports
  `BOOST_VERSION=108700` and `BOOST_LIB_VERSION="1_87"` (exit 0).
- Fresh SimpleWeb's committed CMake is pristine; the automatically patched
  working CMake equals the qualified adapter exactly, with both hashes saved.
  mDNS HEAD is the required qualified SHA.
- Final OFF CTest file has exactly the two safe registrations, API filtered to
  `[security]`, and no generic discovery. Separate default-option reconfigure
  (exit 0) proves ON remains the default and preserves upstream deferred
  registration; `build.ninja` has no `CatchAddTests.cmake` build step. Saved
  files: `CTestTestfile-final-OFF.cmake`, `CTestTestfile-default-ON.cmake`,
  `DEFAULT-ON-wolftests-b12d07c_include.cmake`, `native-default-*`.

Raw artifact hashes and exact resulting source revision are recorded in
`rawhashes.json` and `source-revision.json`. No executable/main or CTest runtime
was invoked to obtain these results.

Artifact root:
[`MWOW-10`](/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-10).
The native developer/CI configure command (no external adapters) is:

```bash
cmake -S /run/media/bazzoozza/work/Worktrees/mirror-wow-wolf-build-fix \
  -B /run/media/bazzoozza/Games/bb-targets/wolf/MWOW-10/native-clean-final \
  -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_STANDARD=20 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DADD_STACKTRACE_INFO=OFF -DTEST_RUST_WAYLAND=OFF -DTEST_DOCKER=OFF \
  -DTEST_VIRTUAL_INPUT=OFF -DTEST_NVIDIA=OFF -DTEST_EXCEPTIONS=OFF \
  -DWOLF_RUNTIME_TEST_DISCOVERY=OFF
```

After Manager combines the frozen security/lifecycle commits and this commit,
the intended build/test commands are (documented, **not executed here**):

```bash
cmake --build <combined-build> --target wolf-protocol-security-tests wolf-api-security-tests -j2
ctest --test-dir <combined-build> -R '^wolf-(protocol|api)-security-tests$' --output-on-failure
# Direct equivalents, using Catch2's main:
<combined-build>/tests/wolf-protocol-security-tests
<combined-build>/tests/wolf-api-security-tests '[security]'
```

Normal CI can omit `-DWOLF_RUNTIME_TEST_DISCOVERY=OFF`; ON remains the default,
and a subsequent intentional CTest run registers the existing upstream cases.
Never run the upstream main as part of these isolated checks.

Installed-Boost package qualification, complete build, GCC/static/Release,
sanitizers, combined security cases and runtime compatibility remain untested
on this revision. Inputtino motion fixes and the other isolated targets owned
by MWOW-6/7 are intentionally not copied into this branch. No OOM fix, complete
BOM or user acceptance is claimed. Exact usage is unavailable; Manager owns the
Jiraia lease and no usage numbers are invented.

## UUID dependency follow-up after combined build review

Manager's combined revision `d57e6b0145fb8733b4ec0634604a453ebc7c2478`
failed compiling `wolf_runner`: `state/config.hpp:4` includes
`boost/uuid/uuid_generators.hpp` (and `uuid_io.hpp` on line 5), but its generated
include search path did not contain the split UUID module. The earlier
configure-only checkpoint did not detect this production compile failure.
Evidence read without changing the combined checkout/build:
`/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-8/native-final/build-shared-san.log`.

The FetchContent fallback now selects **`uuid`** alongside `property_tree` and
adds the real **`Boost::uuid`** to `Boost_LIBRARIES`. The existing PUBLIC link
on `wolf_runner` propagates its official include directory and transitive
dependencies to consumers. Boost 1.87's own UUID target also links `atomic` on
this Clang/Linux configuration; that edge is preserved. No global include,
manual `-I`, host/monolithic overlay or fake target is added. The installed
Boost lookup and its original compiled component requirements remain unchanged.

Checks on the existing MWOW-10 worktree (AST root still has no index; no rebuild):

- Incremental CMake configure/graph only: **exit 0**. FetchContent updates were
  disabled for this light check; no source overrides or external hooks added.
  Actual edges: `wolf_runner -> boost_uuid (Boost::uuid) -> atomic`.
- File API includes the UUID module's source include directory in `wolf_runner`.
  A syntax-only Clang probe using its generated include flags compiles the two
  UUID headers and generator/string-conversion expressions: **exit 0**. It
  neither links nor runs an executable and does not inject an extra include path.
- All **50** distinct direct Boost includes in production `src/` resolve from
  the generated runner search path to the selected Boost 1.87 source tree,
  rather than accidentally falling back to host Boost headers: **PASS**.
- `git diff --check`: **PASS**. No full build/test/main/runtime operation;
  Manager retains the heavy slot and owns the combined rebuild.

Exact command/log/exit: `uuid-configure-command.json`, `uuid-configure.log`,
`uuid-configure-exit.json`; graph: `uuid-targets.dot`; generated target/header
proof including exact syntax-probe argv: `uuid-proof.json`,
`uuid-header-probe.cpp`, `uuid-header-probe.log`, all under the MWOW-10 artifact
root. `uuid-fix-source-revision.json` and `uuid-fix-rawhashes.json` identify this
follow-up. Earlier `source-revision.json`/`rawhashes.json` describe the historical
912e2cd checkpoint; mutable metadata inside its own `native-clean-final/` build
directory was regenerated for this UUID check. Earlier raw configure logs and
source-provenance records remain retained. This follow-up does not claim that
the combined production build or security suites have passed.

## ENet PIC follow-up after combined link failure

Combined `7e3c430a199d8fdc81a2fd983271e7f71afdb539` compiled all production
translation units, then failed linking `libwolf_runner.so`: the pinned ENet
build hardcodes a static `enet` target, whose `host.c.o` was not PIC.
`/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-8/native-final/uuid-build.log`
reports `R_X86_64_PC32` against `enet_host_connect` and `recompile with -fPIC`;
`uuid-combined-commands.json` identifies that revision and exact failed commands.
That actual combined build remains **FAILED** until Manager rebuilds it.

Immediately after `FetchContent_MakeAvailable(enet)`, Wolf now sets the actual
`enet` target's **`POSITION_INDEPENDENT_CODE ON` when `BUILD_SHARED_LIBS` is ON**.
This fixes the static archive's compilation for shared consumption. The original
ENet SHA `44c85e16279553d9c052e572bcbfcd745fb74abf`, static library type, PUBLIC
link to the runner and non-shared branch remain unchanged. No global compiler
flags, sanitizer changes or linker relaxation are introduced. Runtime/test
sources and `tests/CMakeLists.txt` are not changed by this follow-up.

Configure-only check in the existing MWOW-10 build directory: **exit 0**.
File API confirms `enet=STATIC_LIBRARY`, `wolf_runner=SHARED_LIBRARY` and the
real runner-to-ENet dependency. All **9** generated ENet C compile commands,
including `host.c`, contain **`-fPIC`**; global `CMAKE_C_FLAGS` does not.
No object compilation, link, full build, test, main or runtime was executed.
The combined checkout/build was read only; Manager retains the heavy slot.

Exact args/log/exit: `enet-pic-command.json`, `enet-pic-configure.log`,
`enet-pic-exit.json`; graph: `enet-pic-targets.dot`; complete generated ENet
argv/type/pin evidence: `enet-pic-proof.json`. Revision and raw hashes are in
`enet-pic-source-revision.json` and `enet-pic-rawhashes.json`, under the MWOW-10
artifact root. Own mutable CMake metadata was regenerated; earlier checkpoint
manifests remain historical. This evidence does not claim a successful actual
shared/sanitized link or combined security run.
