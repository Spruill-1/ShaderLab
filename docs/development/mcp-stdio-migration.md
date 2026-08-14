# MCP Migration: HTTP → stdio + broker relay

Replaced the embedded HTTP MCP server with a stdio transport fronted by a singleton
broker. **The migration is complete** (see the status line below); this doc is now both
the completion record and the onboarding guide for the MCP subsystem.

**New here? Start with [Picking this up (fresh clone, little context)](#picking-this-up-fresh-clone-little-context)** — it's self-contained: what the pieces are, how to build from a
clone, how to run every test, how to deploy, and where the code lives. The only work
left is the [Manual verification sweep](#manual-verification-sweep-run-once-the-migration-is-complete). Everything below the onboarding section (`## Why` onward) is
design rationale + the nine-step history, kept for reference.

Status: **COMPLETE — all 9 steps done, 2026-08-10.** The embedded HTTP transport is
deleted; the broker (shim → hub → session over named pipes, bodies sealed) is the only
MCP transport. Engine ABI **3**. What remains before the migration can be called fully
signed-off is the **manual verification sweep** at the end of this doc (WinUI window
lifecycle, packaged install/activation, a real MCP client, the in-place-upgrade
sequence) — everything automatable is green: 261 unit tests, broker smoke 26/26,
headless smoke, and the shim-driven `RunTests.ps1` at 40/40 (GUI) / 21/21 (headless)
on WARP. Two throwaway spikes have already settled the platform questions; their
results are recorded in [Settled by spike](#settled-by-spike) so they are not
re-litigated.

---

## Why

The MCP server is a Winsock HTTP listener embedded in every host
(`Engine/Mcp/McpRouter.cpp`, né `McpHttpServer.cpp` until Step 2), default port 47808.

1. **Unaddressable.** The listener scans 10 ports and nothing publishes the bound one,
   so a client config can't reliably find a session, and multiple ShaderLab windows
   can't be told apart at all.
2. **Unauthenticated and in the clear.** Every response carries
   `Access-Control-Allow-Origin: *` on an unauthenticated loopback socket. Any local
   process can drive the graph and observe traffic, including `render_capture_node`
   image payloads.
3. **Wrong transport for the ecosystem.** MCP clients expect stdio.

---

## Target architecture

```
MCP client ──stdio──> shim ─┐
MCP client ──stdio──> shim ─┤ sealed frames
                            \\.\pipe\ShaderLab.mcp.v1.<user>   (hub: blind relay)
              ShaderLab.exe (session) ─┤
        ShaderLabHeadless.exe (session)┘
```

| Component | Role | Packaged? | Sees plaintext? |
|---|---|---|---|
| **Shim** (`--stdio`) | Full MCP front-end: answers `initialize`, owns `list_sessions`/`use_session`, splices `tools/list`, correlates `id`, owns request timeouts | **No** — copied to `%LOCALAPPDATA%\ShaderLab\bin\` | Yes (endpoint) |
| **Hub** (`--hub`) | Blind byte relay. Routes on `{channelId, seq}`; channel liveness only | **Yes** — activation is the only way it survives the client's job object | **No** |
| **Session** | MCP JSON-RPC + route table, engine-side so both hosts get it | Yes | Yes (endpoint) |

### Why encrypt at all

Everything runs in one user session, and same-user isolation is not a hard boundary on
Windows. This is **not** a defence against a local attacker and the plan should not
claim otherwise. The reason is architectural: it makes "the hub is plumbing" a property
of the code rather than a convention — the hub cannot log, cache, dump or inspect a
payload because it does not hold the key, and that stays true when someone later adds
diagnostics to the relay. A 4K inline capture (~33 MB of base64) never enters the
address space of the component most likely to be crash-dumping.

Ephemeral P-256 ECDH → HKDF-SHA256 → AES-256-GCM, via BCrypt. No new dependency.

### Binary pairing — the one enforced boundary

Verify the peer **process**, not its claims, and keep it version-tolerant:

- **Identity** = package family name only. *Not* the install root — it changes per
  version and isn't reliably under `WindowsApps`.
- **Compatibility** = protocol version only, carried in the pipe name (`v1`). Bumped
  only on wire-format breaks, never per release.
- **App / engine ABI version** = informational, surfaced via `list_sessions`, never a
  rejection reason. Comparing it would reject the old-shim/new-hub pairing on **every**
  routine upgrade and destroy the benefit of making the shim update-immune.

---

## Settled by spike

Measured with throwaway packages, not reasoned about. **Platform:** Windows 11
(10.0.26xxx), ARM64. Behaviour is expected to be identical on x64, but the two items
marked ⚠ are worth re-checking if they ever look wrong. (The manifest's declared
floor was `10.0.17763` when these were measured; it has since been raised to
`10.0.22621`, which only narrows the unverified range.)

| Question | Answer |
|---|---|
| Can a detached hub survive the MCP client's job object? | Only via `IApplicationActivationManager`. Plain `CreateProcess` is killed with the job; `CREATE_BREAKAWAY_FROM_JOB` fails `ERROR_ACCESS_DENIED`. |
| Must the hub be a packaged `<Application>`? | Yes — activation needs an AUMID. `AppListEntry="none"` + console subsystem validates and activates fine. |
| Does `ActivateApplication`'s `arguments` reach `argv`? | Yes. **All hub config must travel this way.** |
| Does the launcher's environment reach the hub? | **No** (cwd is `system32`). `SHADERLAB_MCP_*` env vars never reach a production hub. |
| Console window on activation? | **Yes, visible.** The hub must `FreeConsole()` first thing in `--hub` mode. |
| Two activations → one process or two? | **Two.** Redundant hubs are normal, so the election-loser path must be fast, silent and console-free. |
| Named pipe, user-only DACL + `PIPE_REJECT_REMOTE_CLIENTS`, under MSIX? | Works. |
| `GetNamedPipeServerProcessId` from the *client* handle? | ⚠ Works, though the docs imply server-side handles only. Cover it with a unit test. |
| Does an MSIX update kill a **packaged** shim? | **Yes** — hard kill, `exitCode=1`, no console ctrl event, so it can't even emit a final JSON-RPC error. Without `-ForceApplicationShutdown` the update instead **fails** with `0x80073D02 ERROR_PACKAGES_IN_USE`. Note `scripts/Install.ps1` passes the force flag. |
| Does an **unpackaged** shim survive an update? | **Yes, immune.** This is why the shim is copied out of the package. |
| Can the on-disk shim be refreshed while an old one runs? | ⚠ Overwrite-in-place is blocked; **rename-then-write works** and the running process is unaffected. |

---

## Current state

> **Historical snapshot** (mid-migration, ~Steps 2–3). Every residual noted below was
> resolved in a later step — kept for the reasoning trail. For today's state see the
> status line at the top and the per-step ✅ completion notes; for practical commands
> see [Picking this up](#picking-this-up-fresh-clone-little-context).

**Complete:** the activation spike, and repair of `Tests/RunTests.ps1` — the repo's only
MCP regression suite (26 MCP-driven tests). It was not runnable as found: a dead
`$MSBuild` path, an unused `WaitForMcp`, a readiness probe pointed at a
render-dispatched route with a 2 s timeout, and 6 stale tests referencing effects that
no longer exist. Now 33/33.

Also completed, unplanned: two intermittent access violations were root-caused to a
single race (the UI thread reading live `EffectNode`s during canvas paint while the
render worker mutated the graph) and fixed using the pre-existing but unused
`GraphUiSnapshot`, plus a lock on `m_visuals`. This was a prerequisite — the suite
couldn't gate anything while 3 of 4 runs crashed. See decision-log #70; the resulting
threading rules live in `Controls/NodeGraphController.h` and
`.github/copilot-instructions.md`.

**Known residuals of the same defect class** — verified against the live tree
(2026-08-09) and parked here deliberately so Step 7 picks them up with the rest of
the `MainWindow.*` threading work rather than as drive-by fixes now:

- `MainWindow.RenderTick.cpp:126-131` — the 250 ms periodic UI tick reads the
  **live** graph on the UI thread: `m_graph.HasDirtyNodes()`, then
  `m_graph.FindNode(m_selectedNodeId)` and a walk of `selNode->propertyBindings`,
  all outside both the snapshot and `m_graphMutex`. Same shape as the two
  decision-#70 crashes, different caller. Note `GraphUiSnapshot` mirrors per-node
  `dirty` but **not** `propertyBindings`, so the clean fix is a small snapshot
  extension (e.g. a has-bindings flag), not just a call-site swap; the
  alternative — taking `m_graphMutex` shared — buys the worker-tick stall #70
  measured at ~50 ms worst case.
- `MainWindow.xaml.h:284-289` — `m_frameGeneration` is a plain `uint64_t` whose
  declaration comment argues "written only from the render path so a non-atomic
  uint64 is fine", but `OnRenderTick` now reads it on the UI thread
  (`MainWindow.RenderTick.cpp:79`) to gate canvas redraws. Formally a data race
  (in practice benign for aligned 64-bit loads on x64/ARM64); make it
  `std::atomic<uint64_t>` with relaxed ordering and fix the comment, which
  currently asserts a property that is no longer true.
- Dead pre-worker tick path: `MainWindow::RenderTickBody`
  (`MainWindow.RenderTick.cpp:376`, zero callers), `MainWindow::RenderFrame`
  (renders on the **UI** D2D context; only reachable from the dead body), and the
  `MainWindow::CaptureNodeAsPng` / `MainWindow::ReadPixelRegion` wrappers (zero
  callers; superseded by the engine routes, which correctly run on the render
  thread with `ctx.dc = RenderD2DContext()`). Delete in Step 7 — anyone reasoning
  about shutdown ordering or context ownership will otherwise trip over a
  `RenderFrame` that runs on the wrong context.

**Step 1 — route hygiene: complete** (2026-08-10). `/effects` + `/graph/overview` are
engine routes (both hosts serve them); `/graph/rename-node` + `/display/info` are real
app-side routes — rename mutates on the render thread and `TryEnqueue`s the XAML
refresh, and `get_display_info` **stays app-side by decision** (it reads
`RenderEngine::ActiveFormat()`; extending `EngineContext` waits for Step 2's ABI
bump). The `GET /render/pixel/` stub and the phantom `image_stats` tool are gone
(**39** tools now). `RunTests.ps1` gained 6 promoted-route tests — 39/39 green — and
`ShaderLabHeadless --script` now answers `GET /effects` / `GET /graph/overview`, the
first headless `list_effects` this step's verify gate asked for.

**Step 2 — transport-neutral types + rename: complete** (2026-08-10).
`Engine/Mcp/McpTypes.h` holds `Mcp::Response` (+ `noReply` discriminator, honoured as
202-empty over HTTP); `McpHttpServer.{h,cpp}` → `McpRouter.{h,cpp}` via `git mv`; all
43 route lambdas take `(path, query, body)` with the router owning the query split;
`McpRouter::HasRoute()` added; ABI **2**; `EngineContext::getPipelineFormatName`
landed and `/display/info` moved engine-side (headless serves `get_display_info`).
Verified: both platforms build, both hosts pass the ABI check, suite 39/39, and 10
new `McpRouter` unit tests (193 total) pin the query-split contract — the planned
`curl '?since=3'` check could not demonstrate filtering live because **no
MCP-reachable path produces node-log entries any more** (next paragraph), so the
contract is pinned at unit level instead.

**Additional residual, found during Step 2 verification:** the per-node
runtime-error transition logger (`m_nodeLogs[...]` writes around
`MainWindow.RenderTick.cpp:745-766`) lives in the **dead pre-worker `RenderFrame`
path**, so it has silently not run since the v1.7.0 worker migration. Node runtime
errors are still *set* (visible via `graph_get_node`) but never logged; the only
remaining `m_nodeLogs` writers are GUI-native interactions (canvas connect, panel
edit, clock click, file drop). Over MCP, `node_logs` can only ever return entries a
human created first. Fold into Step 6's `node_logs` decision — relocating the route
is pointless without also reviving a producer on the live worker path.

**Step 3 — dispatcher + tool catalog into the engine: complete** (2026-08-10).
`Engine/Mcp/McpJsonRpc.{h,cpp}` (dispatcher) + `Engine/Mcp/McpToolCatalog.{h,cpp}`
(39 declarative rows); the GUI's 376-line inline dispatcher is deleted and
`RegisterJsonRpcEndpoint` on the host's router is the whole integration for both
hosts. `ShaderLabHeadless --serve [--port]` serves the full protocol, and
`RunTests.ps1 -Port` + host-kind self-skipping made the suite CI-runnable — the
"MCP suite vs headless session" step in `ci.yml` now gates every push. All the
stdio-conformance items landed: single-line messages, zero-byte notifications
keyed off an absent id, id echoed on every error path, guarded `params`,
`_setmode` deferred to the actual stdio transport (Step 6), one shared
`Mcp::JsonEscape`. **protocolVersion decision:** pinned to `2025-06-18` — the
revision that *removed* JSON-RPC batching — with an explicit -32600 for batch
arrays; the previously-pinned 2024-11-05 required batching this server never had.
Verified: 211 unit tests (15 dispatcher + catalog), GUI suite 40/40, headless
suite 21/21 with 19 GUI-only tests self-skipping, both platforms build.

**Found during Step 3 (fixed):** registering `POST /` on a headless router meant a
tool with no backing route fell through longest-prefix matching into the
dispatcher itself and read as an id-less notification — a silent fake success,
the `image_stats` failure class resurrected. `McpRouter::HasSpecificRoute`
(catch-all excluded) now guards every tools/call forward; absent tools return an
isError "Tool not available on this host" result. This is also why per-host
tools/list filtering must NOT be attempted via route lookup — the Step 5 shim
splices tools/list per session as planned, and both hosts advertise the full
catalog until then.

**Step 4 — frame codec + crypto + peer identity: complete** (2026-08-10). Three
new pure-unit modules under `Engine/Mcp/`, no IPC yet:
- `McpFrame.{h,cpp}` — `[u32 totalLen][u32 channelId][u64 seq][body]`, LE, 64 MB
  cap. The `{channelId, seq}` header is a distinct clear type from the sealed
  body so the split cannot drift. `TryDecodeFrame` never consumes on a partial
  read, and an over-cap length prefix is an explicit `Oversize` (poison the
  connection) rather than a desync.
- `McpCrypto.{h,cpp}` — ephemeral P-256 ECDH → HKDF-SHA256 → AES-256-GCM via
  BCrypt. Both CNG traps handled and commented: `BCRYPT_KDF_RAW_SECRET` returns
  the secret byte-reversed (flipped to big-endian), and the public blob carries
  a `BCRYPT_ECCKEY_BLOB` header ahead of X‖Y (never sized against the bare
  curve). Two direction keys via HKDF info labels so the GCM nonce is just the
  seq; the clear frame header rides as AAD, turning header tamper or seq desync
  into an `Open()` auth failure.
- `McpPeerIdentity.{h,cpp}` — `GetPackageFamilyName(HANDLE)` (with the
  ERROR_INSUFFICIENT_BUFFER sizing trap noted), pipe PID both directions, and
  `EvaluatePairing` as pure policy: packaged→PFN equality, unpackaged→
  dir+build gated behind `SHADERLAB_MCP_ALLOW_UNPACKAGED=1`, mixed always
  refused (no override).

Verified: +33 unit tests (244 total), both platforms build. Coverage includes
the plan's full list — 40 MB round-trip, truncated/oversize/malformed frames,
sequence desync, tampered ciphertext/tag/AAD, HKDF against RFC 5869 A.1, the
mirrored handshake, and a real loopback named pipe proving
`GetNamedPipeServerProcessId` from the **client** handle works (the ⚠ spike item)
plus peer identity resolving the current process as its own peer.

**Step 5 — hub + shim, zero sessions: complete** (2026-08-10). New
`ShaderLabMcpBroker/` + `.vcxproj` (in `ShaderLab.slnx`): ONE console binary, two
modes. `--hub` FreeConsole()s first, runs the first-instance election with the
prove-the-loss rule (ERROR_ACCESS_DENIED → connect + complete hello; healthy
incumbent → exit 0, otherwise exit 3 with a distinct log line), serves overlapped
per-connection I/O with next-instance-before-serve, verifies every peer via
`McpPeerIdentity` pairing at hello, answers channel-0 control ops, and idle-exits
(`--idle-exit-sec`). `--stdio` is the shim: binary-mode NDJSON, owns `initialize`
(protocol 2025-06-18) + the `list_sessions`/`use_session` tools + hub-op timeouts;
graph tools return a clean isError "No session attached"; logs to
`%LOCALAPPDATA%\ShaderLab\logs\`, stdout carries protocol bytes only. The broker
does NOT link the engine — the Step 4 modules compile directly into the exe.
Manifest gained `uap3`/`desktop` namespaces + `<Application Id="Hub">`
(literal executable, `Windows.FullTrustApplication`, `AppListEntry="none"`, full
VisualElements, no AppExecutionAlias); `CopyBrokerRuntime` mirrors
`CopyEngineRuntime` into the Appx payload. Named-object isolation: `--pipe` /
`SHADERLAB_MCP_PIPE` sets the base name and the readiness event derives from it.
Verified: `Tests/RunBrokerSmoke.ps1` 19/19 (election winner/loser, framing,
no-session shim protocol incl. zero-byte notifications, stdout+stderr hygiene,
idle exit) — wired into CI with the pre-launched-hub caveat documented.

**Found during Step 5 (both fixed, one measured):** the explicit pipe DACL must
include `FILE_READ_EA`/`FILE_WRITE_EA` — `CreateNamedPipe` internally requests
`FILE_GENERIC_READ|WRITE`, and without the EA bits the hub's own next-instance
create fails `ERROR_ACCESS_DENIED` (measured; the smoke caught the hub dying
after its first client). And the shim's `HubConnection` needed rule-of-five move
semantics — the compiler-generated copy duplicated the raw pipe HANDLE and the
temporary's destructor closed it ("Hub connection lost" on the first
post-connect request).

**Step 5 note for Step 6/8:** the unpackaged pairing fallback compares image
DIRECTORIES. Dev binaries live in per-project out dirs
(`…\ShaderLabMcpBroker\` vs `…\ShaderLabHeadless\` vs `…\ShaderLab\`), so
hub↔session pairing across projects in the build tree will refuse as written.
The broker smoke passes because hub and shim are the same exe. Step 6 must
either stage dev binaries into one directory, relax the fallback to the common
`<Platform>\<Config>` root, or accept per-pair env overrides — decide there.

**Step 6 — session client + headless session: complete** (2026-08-10). New
`Engine/Mcp/McpChannel.{h,cpp}` (the shared per-channel `SecureChannel`: P-256
handshake + AES-GCM seal/open, AAD = {channelId,seq}, one home compiled by both
the broker shim and the engine) and `Engine/Mcp/McpSessionClient.{h,cpp}` (written
once against `McpRouter&`: connects to the hub as role=session, serves each sealed
channel request by routing plaintext JSON-RPC through the router's `POST /`
dispatcher, reconnects with capped backoff, one in-flight request per session).
The hub gained a session registry + blind channel relay (`open-channel` allocates
a channelId pairing shim↔session; frames route on channelId only; a dead session
emits a distinct `session-gone`). The shim pins a session (`use_session`, validated
against the live registry), runs the initiator handshake, seals/forwards requests
verbatim (id passes through), and **splices `tools/list`** (shim's 2 tools + the
pinned session's catalog, merged as real JSON values). `ShaderLabHeadless
--mcp-session [--session-id GUID] [--session-label] [--pipe]` wires it to the
existing `HeadlessSink`; the session id is a persisted per-window GUID (generated
when omitted), never an ordinal. Verified: 256 unit tests (+12 channel + pairing),
`RunBrokerSmoke` **26/26** now driving a real WARP headless session end-to-end
(register → `use_session` → spliced `tools/list` → `graph_overview` +
`graph_add_node` through the sealed relay → `session_gone` on session kill).

**Step 5 directory-pairing note resolved:** the unpackaged fallback now accepts a
shared parent dir (both binaries under `<Platform>\<Config>\`), not just an exact
dir, so hub↔session pairing works across sibling per-project out dirs. Still gated
behind `SHADERLAB_MCP_ALLOW_UNPACKAGED=1`, still requires matching build id, still
never engages packaged. Two new pairing tests pin accept-sibling / refuse-different-root.

**`node_logs` decision (deferred item from Steps 2/6):** left in the catalog,
served GUI-only. On a headless session `tools/call node_logs` returns the standard
isError "Tool not available on this host" (via `HasSpecificRoute`) — correct, not a
hang. Reviving a worker-thread log producer is out of scope for the transport
migration; tracked as a separate cleanup. The smoke asserts nothing on `node_logs`.

**Step 7 — GUI session client: complete** (2026-08-10). The same `McpSessionClient`
is wired into `MainWindow` (`m_sessionClient` + `m_sessionThread`, started with the
MCP toggle / autostart, alongside the still-live HTTP listener). It serves through
`m_mcpServer`'s router, so tool calls marshal to the render worker via
`GuiEngineCommandSink::Dispatch` and fire the 8 live event hooks. The correctness
work the plan flagged:
- **`RenderThreadDispatcher` fail-fast**: `Shutdown()` / `ResetConsumer()` now invoke
  queued items with `cancelled=true` so pending `DispatchSync` promises FAIL
  immediately instead of eating a 30 s timeout; a `DispatchSync` on an
  already-shut-down queue also fails fast. Queue element is now
  `std::function<void(bool)>`. +3 unit tests.
- **`MainWindow::DispatchSync`** now checks `TryEnqueue`'s return and throws
  immediately when the DispatcherQueue is shutting down (was discarded → 30 s per
  request during close).
- **Timeout ladder** in one header (`Engine/Mcp/McpTimeouts.h`, `static_assert`-ed
  ordering): render closure < `DispatchSync` < shim < client. Wired into the sink's
  render-rung wait and the shim's session-wait.
- **Shutdown ordering**: `~MainWindow` stops the session FIRST (Stop → close pipe →
  join) while the worker is still alive, THEN the HTTP listener, THEN
  `m_renderDispatcher.Shutdown()` + worker join — no request stranded on a joined
  worker.
- **Adapter-switch gating**: `m_adapterSwitchInProgress` makes the sink return 503
  for the whole `SwitchAdapter` teardown/rebuild window (reset on every exit path).
- **Toolbar**: the toggle exposes this window to MCP; the label shows
  `MCP: off` / `MCP: on (session + :port)`.

**Two design points surfaced and resolved here** (both needed before a GUI session
was reachable at all):
- **Role-aware pairing.** Strict binary pairing is enforced on SESSION registration
  (a session drives real graphs) but NOT on a SHIM — the shim is unpackaged while
  the production hub is packaged (an expected mix that the old strict rule refused),
  and shim↔session payloads are sealed end-to-end anyway. The session client still
  strictly verifies the hub.
- **One default pipe name.** `DefaultPipeBaseName()` (`McpPeerIdentity`, compiled by
  both binaries) is the single SID-derived default so hub, shim and every session
  client meet without a `--pipe` override — the broker and session client had
  diverged (`…<SID>` vs `…default`).

**Residual sweep (complete).** The two UI-thread live-graph reads in the 250 ms tick
now read under a shared `m_graphMutex` lock and act after release; `m_frameGeneration`
is `std::atomic<uint64_t>`; and the dead pre-worker tick path is **deleted** —
`MainWindow::RenderTickBody` + `RenderFrame` (RenderTick.cpp, ~500 lines) and the
`MainWindow::CaptureNodeAsPng` / `ReadPixelRegion` shims (xaml.cpp, ~65 lines), all
unreferenced since v1.7.0. The `render_capture_node` / `read_pixel_region` routes were
re-verified over MCP afterward (they are engine-side, using `Rendering::CaptureNodeAsPng`
/ `Rendering::ReadPixelRegion` on the render worker). Build clean, no new warnings.

**Verify (plan says manual).** Automated what was feasible: unit suite 261 (dispatcher
fail-fast), both platforms build, the HTTP MCP suite still 40/40 against the GUI
(no regression), and a **new GUI-as-session end-to-end** — activate the packaged hub,
the running GUI registers a GUID session, an unpackaged shim lists/pins it and drives
`graph_add_node` + `graph_overview` + `list_gpus` (a GUI-only tool, proving the request
reached the real GUI host) through the render worker. `session_gone`, GPU-switch-mid-
request, and fully-graceful two-window close remain manual.

**Toolbar / activity items deferred to Step 8:** the stdio export snippet (the shim
isn't distributed until Step 8) and the richer `MCP: session 1 of 2` / `MCP: no hub`
label (needs a hub round-trip on the UI tick); `ActivityCallback`'s `peerAddress →
clientId` rename rides with the Step 9 HTTP removal.

**Not started:** Steps 8–9.

---

## Picking this up (fresh clone, little context)

Self-contained. If you just cloned the repo and know nothing else about it, read this
section top to bottom.

### What this is

ShaderLab is a Windows desktop app (WinUI 3 / C++/WinRT, no C#) for authoring and
debugging Direct2D / D3D11 shader effects. It exposes an **MCP** (Model Context
Protocol) server so AI agents can drive its node-based effect graph. This migration
replaced the app's old embedded **HTTP** MCP server with a **stdio broker**, because the
HTTP one was unaddressable across multiple windows, unauthenticated on a loopback port,
and the wrong transport for MCP clients. It is **done**; the only work left is the
[Manual verification sweep](#manual-verification-sweep-run-once-the-migration-is-complete).

### The moving parts (glossary)

Four executables, all built from `ShaderLab.slnx`:

- **`ShaderLab.exe`** — the packaged WinUI app. Each open window registers itself as an
  MCP **session**.
- **`ShaderLabEngine.dll`** — the shared native engine (graph model, evaluator, and all
  the `Engine/Mcp/*` MCP code). Both hosts link it.
- **`ShaderLabHeadless.exe`** — console host, no WinUI. `--mcp-session` registers a
  headless session (what CI and no-GUI testing use); also does PNG render / pixel
  readback / `--script` batch mode.
- **`ShaderLabMcpBroker.exe`** — the broker. `--hub` = the singleton relay; `--stdio` =
  the shim an MCP client launches. One binary, two modes. Deliberately does **not** link
  the engine (the hub must start in milliseconds).

Transport chain: **MCP client → shim (stdio) → hub (named pipe, blind relay) → session
(engine routes).** Shim↔session bodies are AES-256-GCM sealed, so the hub relays but
can't read them. Sessions are GUID-identified; a client calls `list_sessions`, then
`use_session <id>` to pick a window, then drives tools.

- **The pipe**: `\\.\pipe\ShaderLab.mcp.v1.<user-SID>` by default (`DefaultPipeBaseName()`
  in `Engine/Mcp/McpPeerIdentity.cpp` — the one source of truth all four binaries share).
  Override with `--pipe <base>` or the `SHADERLAB_MCP_PIPE` env var.
- **Pairing**: the hub accepts a **session** only if its package family name matches
  (production). For dev/CI, where everything is unpackaged, it falls back to "same build
  id + shared config root" — but **only** when `SHADERLAB_MCP_ALLOW_UNPACKAGED=1` is set,
  so it can never engage in an installed configuration. The **shim** is accepted
  regardless of packaging (role-relaxed; its payloads are sealed to the session anyway).

### Build from a fresh clone

Full prerequisites are in [build.md](build.md); two things bite a fresh clone specifically:

1. **Init the submodules first**, or the build stops with a "run this command" error:
   ```pwsh
   git submodule update --init --recursive     # exprtk + miniz are submodules
   nuget restore ShaderLab.slnx -SolutionDirectory .
   ```
2. **On an ARM64 host you MUST use the ARM64-native MSBuild** — the default 32-bit one
   silently picks a 32-bit compiler that runs out of memory on the large generated files:
   ```pwsh
   & 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\arm64\MSBuild.exe' `
       ShaderLab.slnx /p:Configuration=Debug /p:Platform=ARM64 /m
   ```
   On an **x64 host**, plain `msbuild ShaderLab.slnx /p:Configuration=Debug /p:Platform=x64 /m`
   is fine. (Details: build.md → "Building ARM64 on an ARM64 host".)

Outputs land in `<Platform>\<Config>\<Project>\` (e.g. `ARM64\Debug\ShaderLabTests\`).

### Run the automated tests

Substitute your `<Platform>` (`ARM64` / `x64`) and `<Config>` (`Debug` / `Release`).
Suites 1–4 need **no packaging and no desktop** — this is what CI runs:

```pwsh
# 1. Unit suite — 261 tests on WARP; self-contained (routing, crypto, frame codec,
#    per-channel handshake, dispatcher fail-fast, peer pairing, HLSL math bench).
<Platform>\<Config>\ShaderLabTests\ShaderLabTests.exe --adapter warp

# 2. Broker smoke — 26 checks: hub election, framing, sealed session round-trip,
#    session_gone, idle exit, stdout hygiene.
pwsh -NoProfile -File Tests\RunBrokerSmoke.ps1 -Configuration <Config> -Platform <Platform>

# 3. Headless render smoke — PNG + FP32 pixel readback + a 7-step analysis script.
pwsh -NoProfile -File Tests\RunHeadlessSmoke.ps1 -Configuration <Config> -Platform <Platform>

# 4. MCP integration suite (40 tests) over the broker, against a HEADLESS session.
#    GUI-only tests self-skip (the pinned session's label is "headless"). This is
#    exactly the "MCP suite vs headless session" CI step.
$env:SHADERLAB_MCP_ALLOW_UNPACKAGED = '1'
$bin  = "<Platform>\<Config>"
$pipe = "ShaderLab.mcp.dev.$([guid]::NewGuid().ToString('N'))"
$hub  = Start-Process "$bin\ShaderLabMcpBroker\ShaderLabMcpBroker.exe" `
          -ArgumentList '--hub','--pipe',$pipe,'--idle-exit-sec','600' -PassThru
$sess = Start-Process "$bin\ShaderLabHeadless\ShaderLabHeadless.exe" `
          -ArgumentList '--graph','Tests\fixtures\test_cli_basic.json','--mcp-session', `
          '--pipe',$pipe,'--adapter','warp' -PassThru
try   { pwsh -NoProfile -File Tests\RunTests.ps1 -Pipe $pipe -Adapter warp }
finally {
  Stop-Process -Id $sess.Id, $hub.Id -Force -ErrorAction SilentlyContinue
  Remove-Item Env:\SHADERLAB_MCP_ALLOW_UNPACKAGED -ErrorAction SilentlyContinue
}
```

To run the MCP suite against the **GUI** (fuller coverage — the GUI-only tests actually
execute instead of skipping), deploy + launch the app (next section), then:

```pwsh
$aumid = "$((Get-AppxPackage ShaderLab).PackageFamilyName)!Hub"
pwsh -NoProfile -File Tests\RunTests.ps1 -HubAumid $aumid
```

The suite starts a shim; `-HubAumid` lets the shim activate the packaged hub, the running
GUI's session registers, and it gets pinned. No `-Pipe` = the default per-user pipe.

### Deploy + launch the packaged app

`ShaderLab.exe` is a packaged MSIX app. Register the loose-file layout, launch by
activation:

```pwsh
# Register from the LAYOUT ROOT, never from AppX\ (that subfolder holds stale artifacts).
Add-AppxPackage -Register <Platform>\<Config>\ShaderLab\AppxManifest.xml
explorer.exe "shell:AppsFolder\$((Get-AppxPackage ShaderLab).PackageFamilyName)!App"
```

Traps (all hit during this migration):
- Register fails `0x80073D02` ("in use") → kill running `ShaderLab` processes first.
- Register fails `0x80070490` ("Indexed state handler") after a manifest change →
  `Remove-AppxPackage` the old registration, then re-register (re-registering the same
  version over a *different* layout is a silent no-op that keeps the old path).
- A direct `Start-Process ShaderLab.exe` aborts with a CRT "abort() has been called"
  dialog under packaging — always launch by activation.
- A titleless lingering `ShaderLab.exe` process is a hung shutdown — kill it; it blocks
  redeploys.
- MCP auto-starts a **session** on launch (via `%LOCALAPPDATA%\ShaderLab\config.json`
  = `{"mcp": true}`, or the toolbar toggle). There is **no HTTP port** to poll —
  readiness = the session shows up in `list_sessions` (the suite polls this for you).
  On launch the GUI also copies the shim to `%LOCALAPPDATA%\ShaderLab\bin\`.

### Where the MCP code lives

| File | Role |
|---|---|
| `Engine/Mcp/McpRouter.{h,cpp}` | Pure route registry — `AddRoute` / `RouteRequest` (longest-prefix, owns the query split) / `HasRoute`. No transport. |
| `Engine/Mcp/McpTypes.h` | `Mcp::Response` (+ `noReply`) and the shared `JsonEscape` / `WideToUtf8`. |
| `Engine/Mcp/McpJsonRpc.{h,cpp}` | JSON-RPC dispatcher: `initialize` / `tools/*` / `resources/*` / `ping`. Registered on the router by both hosts. |
| `Engine/Mcp/McpToolCatalog.{h,cpp}` | Declarative 39-tool table (list JSON + route mapping + arg mode). |
| `Engine/Mcp/EngineMcpRoutes.{h,cpp}` | 25 engine-pure routes + `IEngineCommandSink` + `EngineContext`. |
| `Engine/Mcp/McpFrame.{h,cpp}` | Wire frame codec: `[len][channelId][seq][body]`, 64 MB cap. |
| `Engine/Mcp/McpCrypto.{h,cpp}` | P-256 ECDH → HKDF-SHA256 → AES-256-GCM (BCrypt). |
| `Engine/Mcp/McpChannel.{h,cpp}` | Per-channel `SecureChannel`: handshake + seal/open. |
| `Engine/Mcp/McpPeerIdentity.{h,cpp}` | Peer identity, `EvaluatePairing`, `DefaultPipeBaseName`. |
| `Engine/Mcp/McpSessionClient.{h,cpp}` | Registers a session with the hub; serves sealed requests via the router. Used by GUI + headless. |
| `Engine/Mcp/McpTimeouts.h` | The timeout ladder (render < DispatchSync < shim < client). |
| `ShaderLabMcpBroker/Main.cpp` | The `--hub` relay + `--stdio` shim (compiles the `Mcp{Frame,Crypto,PeerIdentity,Channel}` TUs directly). |
| `MainWindow.McpRoutes.cpp` | 16 app-side routes, `GuiEngineCommandSink`, session start/stop + shim distribution. |
| `Tests/RunTests.ps1` / `RunBrokerSmoke.ps1` | The shim-driven integration suite / the broker smoke. |

### Diagnosing a crash without a debugger

The dev box used for this work had no `cdb`/WinDbg. WER's Application-log event 1000
gives a `Fault offset`, which is an RVA; `dbghelp.dll` plus the shipped PDB resolves it
to symbol + line offline. Confirm the event's `time stamp` matches the PE
`TimeDateStamp` first, or you resolve against the wrong layout. Use
`SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME)` — **not** `SYMOPT_DEFERRED_LOADS`,
which silently yields no symbols — and pass the real `SizeOfImage` to
`SymLoadModuleExW`. Note the same bug can surface as two different offsets depending on
inlining; resolve every distinct offset before assuming multiple defects.

---

## Step 1 — Route hygiene *(HTTP still live)* — ✅ done 2026-08-10

`MainWindow.McpRoutes.cpp`, `Engine/Mcp/EngineMcpRoutes.cpp`

- Delete the `GET /render/pixel/` stub. It returns "coming soon" but first touches
  `D2DDeviceContext()` / `CreateBitmap()` on the caller's thread with no dispatch, and
  `std::stof`s unvalidated input.
- Promote the 4 inline `tools/call` handlers to real routes:
  - `graph_overview` → `IEngineCommandSink::Dispatch` (render thread). Currently reads
    `m_graph` on the UI thread.
  - `graph_rename_node` → mutate on the render thread, then fire an event hook that
    `TryEnqueue`s the UI work. **Do not** route it straight through `Dispatch`: it calls
    `RebuildLayout` / `PopulatePreviewNodeSelector` / `PopulateAddNodeFlyout`, which
    touch XAML, and the render worker is MTA → `RPC_E_WRONG_THREAD`. Worse,
    `winrt::hresult_error` does not derive from `std::exception`, so it escapes the
    handler and surfaces as a generic 500 *after* the rename has committed.
  - `list_effects` → genuinely engine-pure; move to `EngineMcpRoutes.cpp`.
  - `get_display_info` → **not** engine-pure; it reads
    `m_renderEngine.ActiveFormat().name` and `EngineContext` has no `RenderEngine`.
    Either extend `EngineContext` (an ABI change — defer to Step 2) or leave it
    app-side. Decide explicitly.
- Drop `image_stats`. It maps to `/render/image-stats`, removed by decision #63 — but
  it does **not** 404. Routing is longest-**prefix**, so it falls through to the
  `POST /` catch-all, re-enters the JSON-RPC dispatcher, finds no `method`, and returns
  **HTTP 200** with a nested error. `isError` is `statusCode >= 400`, so the agent sees
  a successful tool result containing an error.

**Verify:** `RunTests.ps1` 33/33 over HTTP. `list_effects` should now answer from
`ShaderLabHeadless --script`, which is a new capability worth smoke-testing.

> **Done.** 39/39 (the suite gained `Route.RenameNode`, `Route.RenameNodeMissing`,
> `Route.GraphOverview`, `Route.ListEffects`, `Route.DisplayInfo`,
> `Route.ImageStatsRemoved`), and the headless script smoke returns 200 for both
> `GET /effects` and `GET /graph/overview`. Route placement decisions:
> `list_effects`/`graph_overview` engine-side, `rename-node`/`display/info` app-side
> (rename needs XAML refresh and no sink hook exists pre-ABI-bump; display info reads
> `RenderEngine`). `/context`'s endpoint table no longer advertises the deleted
> pixel stub.

---

## Step 2 — Transport-neutral types + rename *(one-way)* — ✅ done 2026-08-10

New `Engine/Mcp/McpTypes.h`; `McpHttpServer.{h,cpp}` → `McpRouter.{h,cpp}`

- Extract `ShaderLab::Mcp::Response`, un-welding the engine ABI from the transport
  header. **Add a no-reply discriminator** — over stdio there is no way to distinguish
  "reply with an empty body" from "send nothing", and an empty line is not valid JSON.
- Change `Handler` to `(path, query, body)` and strip `?` for **matching only**.
  `node_logs` depends on the raw query reaching its handler, and the regression is
  silent: `sinceSeq` stays 0 and the route returns the entire log every poll rather
  than erroring. A third caller is easy to miss — `ShaderLabHeadless/Main.cpp` passes
  arbitrary user-supplied paths straight into `RouteRequest`.
- Add `McpRouter::HasRoute()`. Bump `SHADERLAB_ENGINE_ABI_VERSION`.
- Extend `EngineContext` here if Step 1 deferred `get_display_info`.

> ~39 `AddRoute` lambdas change signature and every later step is written against the
> new one. "Revertable" applies to the **transport**, not to this step. Every
> subsequent step that adds exported engine symbols needs its own ABI bump.

**Verify:** builds on both platforms; the ABI check passes in both hosts;
`curl 'localhost:47808/node/1/logs?since=3'` now honours `since` (a fix, not a
regression).

> **Done.** ARM64 + x64 build clean; GUI aborts-on-mismatch check and headless
> banner both report ABI 2; suite 39/39. The `?since=` fix is pinned by 10
> `McpRouter` unit tests (query reaches handler, path stripped for match,
> longest-prefix with query present, `HasRoute`, 404, `noReply`) because the live
> curl can't show filtering — no MCP-drivable log producer exists post-1.7.0 (see
> *Current state*). `get_display_info` additionally moved engine-side here, per
> Step 1's deferral.

---

## Step 3 — MCP dispatcher + tool catalog into the engine — ✅ done 2026-08-10

New `Engine/Mcp/McpJsonRpc.{h,cpp}`, `Engine/Mcp/McpToolCatalog.{h,cpp}`

- Move the dispatcher out of `MainWindow.McpRoutes.cpp`; the GUI's `POST /` becomes a
  thin delegate. This proves the relocation over the still-live HTTP transport before
  any IPC exists.
- The catalog is a declarative table, but **`responseMode` is a function of the
  response, not the tool**: `graph_snapshot` / `render_capture_node` repack to MCP image
  content only if `inline == true` **and** status 200 **and** the body re-parses **and**
  it has both `base64` and `mimeType`; otherwise control falls through to the generic
  text wrapper. The table needs an argument predicate plus a fallback. Also awkward:
  `node_logs` (arg `sinceSeq` → query `since`, default 0), `graph_load_json` (unwrap a
  named string field into the raw body), `graph_snapshot` (args consumed twice).
- **stdio conformance — all of these are currently violated:**
  - One JSON message per line, no embedded newlines. `initialize`, `tools/list` and
    `resources/list` are multi-line raw string literals today. Invisible over HTTP,
    fatal over stdio.
  - Notifications emit **zero bytes**, and detection keys off an **absent `id`**, not a
    `notifications/` prefix.
  - Never `id: null` on an error path. Over one multiplexed stream it matches no
    pending request and the client hangs until its own timeout. Also guard the
    unchecked `GetNamedObject(L"params")`.
  - `_setmode(_fileno(stdout), _O_BINARY)` — text mode translates `\n` to `\r\n`, so a
    string-level "no `\r`" assertion passes while the wire bytes disagree.
  - Unify the **three** divergent JSON escapers; raw control characters in HLSL error
    text currently produce invalid JSON.
- Decide `protocolVersion` deliberately. The code pins `2024-11-05` but rejects
  batching, which that revision requires.
- Preserve `resources/list` / `resources/read` — a second URI→path table the catalog
  schema doesn't cover. Note `shaderlab://context` maps to a GUI-only route and will
  404 on a headless session.
- `tools/list_changed` is **mandatory**, not optional: the GUI registers ~16 routes the
  engine does not, so `use_session` from a GUI to a headless session genuinely changes
  the tool set — no version skew required.

> Do **not** build a `HasRoute`-based catalog coverage test. Prefix semantics match
> everything against `POST /`; exact semantics fail six *working* tools that are
> sub-paths multiplexed inside prefix handlers (`/graph/save`, `/graph/node/{id}`,
> `/registry/effect/{name}`, `/node/{id}/logs`, `/analysis/{id}`, `/effect/hlsl/{id}`).
> Use a **live round-trip test** instead: drive every catalog tool against a real
> headless session and assert a correctly-shaped result.

**Verify:** `RunTests.ps1` green over HTTP. Headless can now serve `tools/call`, so
**wire `RunTests.ps1` into CI at this step** — this is the first point at which it can
gate, because CI has no interactive desktop.

> **Done.** GUI 40/40 (the suite gained `Route.CatalogRoundTrip`, the live
> round-trip this step prescribes: every advertised tool driven with safe canned
> args, asserting a well-formed envelope); headless 21/21 + 19 GUI-only
> self-skips via the health route's new `host` field; `ci.yml` runs the suite
> against `ShaderLabHeadless --serve` on WARP in both Debug and Release. The
> awkward catalog cases landed as predicted: `node_logs` (NodeLogs arg mode →
> `?since=` query), `graph_load_json` (UnwrapField), and the image-repack
> predicate as a function of the response with a text fallback.

---

## Step 4 — Frame codec, crypto, peer identity *(no IPC yet)* — ✅ done 2026-08-10

New `Engine/Mcp/McpFrame.{h,cpp}`, `McpCrypto.{h,cpp}`, `McpPeerIdentity.{h,cpp}`

- Frames: 4-byte little-endian length + UTF-8 payload. The header carries
  `{channelId, seq}` in the clear (the hub legitimately routes on it); the body is
  sealed. Keep that split explicit in the type so it cannot drift.
- 64 MB cap. A 4K inline capture is ~33 MB of base64; 8K would be ~130 MB, so either
  cap resolution server-side or fail explicitly rather than desyncing.
- Crypto: BCrypt ephemeral **P-256**. X25519 was rejected — CNG named-curve support was
  unverified at the OS floor declared at the time (`10.0.17763`; since raised to
  `10.0.22621`) and it buys nothing against an empty threat model. Two traps: `BCryptDeriveKey` with `BCRYPT_KDF_RAW_SECRET` returns the
  secret **byte-reversed**, and CNG's `ECCPUBLICBLOB` carries a header, so don't size
  buffers against the raw curve.
- Peer identity: `GetNamedPipeClientProcessId` / `ServerProcessId` → `OpenProcess`
  (`PROCESS_QUERY_LIMITED_INFORMATION` suffices, no `SeDebugPrivilege` for same-account
  targets) → **`OpenProcessToken`** → `GetPackageFamilyNameFromToken`. Or use
  `GetPackageFamilyName(HANDLE)` and skip the token step. Unpackaged peers return
  `APPMODEL_ERROR_NO_PACKAGE`; the sizing call returns `ERROR_INSUFFICIENT_BUFFER`, not
  success. The PID-reuse creation-time check is a sanity check, not a guarantee — say so
  in the comment.
- **Unpackaged fallback**, required for dev and CI: when *both* peers lack package
  identity, fall back to "same image directory + matching build id", gated behind
  `SHADERLAB_MCP_ALLOW_UNPACKAGED=1` so it can never silently engage in an installed
  configuration. Mixed packaged/unpackaged is always refused.

**Verify:** unit tests — 40 MB round-trip, truncated and oversize frames, sequence
desync, tampered ciphertext (GCM tag), full handshake, and peer identity resolved
against the current process as its own peer (which also proves the client-handle call
actually works).

> **Done.** `TestMcpFrameCrypto` + `TestMcpPeerIdentity` in `Tests/TestRunner.cpp`
> (33 tests). Every listed check plus: HKDF pinned to RFC 5869 A.1, tampered AAD
> and wrong-direction-key rejection, `EncodeFrame` refusing an over-cap body, and
> the full pairing matrix (packaged same/diff PFN, unpackaged gated/ungated/
> dir-mismatch/build-mismatch, mixed-always-refused). The loopback pipe test
> exercises **both** `GetNamedPipeClientProcessId` (server handle) and
> `GetNamedPipeServerProcessId` (client handle) — the latter is the ⚠ spike item.
> All modules link `bcrypt.lib`; no new external dependency.

---

## Step 5 — Hub + shim, zero sessions — ✅ done 2026-08-10

New `ShaderLabMcpBroker/` + `.vcxproj` (add to `ShaderLab.slnx`); `Package.appxmanifest`

- **Manifest:** add `uap3` and `desktop` namespaces; add a second
  `<Application Id="Hub">` with `Executable="ShaderLabMcpBroker.exe"` (literal —
  `$targetnametoken$` only substitutes for the primary app),
  `EntryPoint="Windows.FullTrustApplication"`, `AppListEntry="none"`, and a **full**
  `<uap:VisualElements>` — the logo attributes are mandatory even when the entry is
  hidden, and omitting them fails package validation. **No `AppExecutionAlias`** (see
  Step 8).
- **Packaging:** payload target mirroring `CopyEngineRuntime`, which must run
  `BeforeTargets="_ComputeAppxPackagePayload"` or the package builds clean with a
  missing exe. `ProjectReference` with `ReferenceOutputAssembly=false` **and
  `LinkLibraryDependencies=false`** (an exe produces no import lib). Do not link
  `ShaderLabEngine.lib` — it would drag D3D/MF into a process that should start in
  milliseconds and never touches a GPU.
- Hub calls `FreeConsole()` first thing. All config arrives via `arguments`, never the
  environment.
- **Election:** never treat `ERROR_ACCESS_DENIED` from `FILE_FLAG_FIRST_PIPE_INSTANCE`
  as a bare "I lost" signal — it also means "parameters differ from the existing
  instance" (exactly the stale-hub-after-update case) and "genuine DACL denial". Prove
  the loss by connecting and completing `hello`; otherwise exit non-zero with a
  distinct reason.
- The readiness event is manual-reset and **stays signalled after a hub dies**. Add a
  wait timeout and a polling fallback, or a session will wait forever on a corpse.
- **DACL footgun:** `FILE_CREATE_PIPE_INSTANCE` shares a bit with `FILE_APPEND_DATA`,
  so a DACL written with `GENERIC_WRITE` lets any same-user process create another
  instance of your pipe. Use individual rights.
- Overlapped I/O on both ends; one writer per pipe; create the next pipe instance
  before serving the current one. `CancelIoEx` returns `ERROR_NOT_FOUND` when the read
  already completed, and the `OVERLAPPED` plus its buffer must outlive the completion
  packet even after a successful cancel — freeing at cancel-return is the classic UAF.
- The **shim** owns `initialize`, `list_sessions` / `use_session`, `id` correlation and
  **request timeouts**. The hub cannot time out by `reqId` — that lives inside the
  sealed body — and dropping a frame would break the sequence counter.
- **stdout carries JSON-RPC frames only.** A stray `printf` corrupts every session and
  is painful to diagnose. Logs go to `%LOCALAPPDATA%\ShaderLab\logs\`.
- `SHADERLAB_MCP_PIPE` alone does not isolate CI — the launch mutex and readiness event
  are also global named objects and need the same treatment.

**Verify:** new `Tests/RunBrokerSmoke.ps1` — election, framing, idle exit, stdout
hygiene, `initialize` / `tools/list` with no session attached. CI must **pre-launch the
hub** (no unpackaged activation path exists), so CI does not cover election — document
that gap rather than pretending otherwise.

> **Done.** 19/19 smoke checks; in CI for Debug + Release. Clarification on the
> gap: the smoke DOES cover the unpackaged first-instance election (two `--hub`
> launches racing the same pipe; loser proves the incumbent via hello and exits
> 0). What no automation covers is **activation-based** launch of the packaged
> hub (`IApplicationActivationManager` → AUMID `…!Hub`) and the packaged↔packaged
> pairing path — both manual, both spike-proven mechanisms.

---

## Step 6 — Session client + headless session — ✅ done 2026-08-10

New `Engine/Mcp/McpSessionClient.{h,cpp}`; `ShaderLabHeadless/Main.cpp`

- Written once against `McpRouter&` and used by both hosts. Handshake, reconnect with
  backoff, one in-flight request per session.
- Session id must be a **persisted per-window GUID**, not an ordinal. After a hub
  restart, windows reconnect in nondeterministic order and an ordinal can silently
  repoint a pinned client at a different graph. A dead pinned session returns a distinct
  `session_gone`; never silently re-route.
- `ShaderLabHeadless --mcp-session` wires it to the existing `HeadlessSink` and route
  registry.
- Drop `node_logs` from the smoke assertions, or relocate the route — it reads a
  `MainWindow` member and a headless session cannot serve it. **Step 2 finding:**
  the route's data source is nearly dry anyway — the runtime-error transition
  logger died with the v1.7.0 worker migration (see *Current state*), so decide
  producer revival and route placement together.

**Verify:** the full `RunBrokerSmoke.ps1` in CI on WARP, end-to-end through real engine
routes. This retires election, framing, crypto and reconnect risk. It does **not**
retire GUI integration risk: `HeadlessSink::Dispatch` is a direct synchronous call with
no DispatcherQueue, no render dispatcher, no XAML, and all 8 event hooks are no-ops.

> **Done.** `RunBrokerSmoke.ps1` 26/26 (13 new session checks incl. the sealed
> `graph_overview`/`graph_add_node` round-trips and `session_gone`), in CI on WARP.
> As the plan predicts, this retires transport risk but NOT GUI-integration risk —
> Step 7 wires the same `McpSessionClient` into `MainWindow` where `Dispatch`
> marshals to the render worker and the 8 event hooks are live, which is where the
> shutdown-ordering / timeout-ladder work lands.

---

## Step 7 — GUI session client — ✅ done 2026-08-10

`MainWindow.*`, `Controls/*`

- **Shutdown ordering.** `~MainWindow` currently calls `m_mcpServer->Stop()` on the UI
  thread, while ~14 routes block *on* the UI thread via `MainWindow::DispatchSync`.
  Putting the session join in the same place reproduces a 30 s stall rather than fixing
  it. Session `Stop()` must be: reject-new → `bye` → `CancelIoEx` → **join**, before
  `m_renderDispatcher.Shutdown()`.
- `MainWindow::DispatchSync` **discards `TryEnqueue`'s return value**. Once the
  DispatcherQueue is shutting down the event never fires and every request eats its
  full 30 s timeout.
- `RenderThreadDispatcher::Shutdown()` / `ResetConsumer()` clear the queue **without
  failing the pending promises**, so no timeout ladder is enforceable until they do.
- `SwitchAdapter` joins the worker and calls `ResetConsumer()`, silently dropping queued
  closures — gate the session to 503 while a switch is in progress. Note a user clicking
  the GPU dropdown mid-request hits this too.
- Put the timeout ladder in one header: render < `DispatchSync` < shim < client.
- Toolbar: the toggle means "expose this window to MCP"; the label shows
  `MCP: no hub` / `MCP: session 1 of 2` / `MCP: off`; the export button emits the stdio
  snippet. `ActivityCallback`'s `peerAddress` becomes `clientId`.
- Sweep the residuals listed in [Current state](#current-state) while in this
  code: the two UI-thread live-graph reads in the periodic UI tick, the
  non-atomic `m_frameGeneration`, and the dead pre-worker tick path
  (`RenderTickBody` / `RenderFrame` / the `MainWindow::CaptureNodeAsPng` +
  `ReadPixelRegion` wrappers).

**Verify:** manual — two ShaderLab windows, session routing via `use_session`, a GPU
switch mid-request, clean shutdown, and every tool exercised.

> **Done.** Automated a GUI-as-session end-to-end (packaged hub via AUMID → running
> GUI registers a GUID session → unpackaged shim lists/pins it → `graph_add_node` /
> `graph_overview` / `list_gpus` through the render worker); HTTP suite still 40/40
> against the GUI; 261 unit tests incl. dispatcher fail-fast. The two-window /
> GPU-switch-mid-request / graceful-close cases stay manual (WinUI window lifecycle
> isn't scriptable here). Role-aware pairing + the shared default pipe name were
> resolved as part of this step — see the completion note above.

---

## Step 8 — Shim distribution — ✅ done 2026-08-10

`MainWindow` startup, `scripts/Install.ps1`, `.mcp.json`

> **Done.** `MainWindow::EnsureShimDistributed()` copies `ShaderLabMcpBroker.exe`
> (the package payload, next to `ShaderLab.exe`) to
> `%LOCALAPPDATA%\ShaderLab\bin\` on every MCP start, **rename-then-write**: an
> existing copy is `MoveFileEx`'d aside (works while a shim runs from it — the
> process keeps its image) and a fresh binary lands at the canonical path; stale
> `.old` files are reaped best-effort. `MainWindow::HubAumid()` derives
> `<PackageFamilyName>!Hub`. The shim gained `--hub-aumid`: when no hub answers,
> `ShimState::EnsureHub` **activates the packaged hub** via
> `IApplicationActivationManager` (`--hub --pipe <base>` so it binds the shared
> default pipe) and polls `WaitNamedPipe` before retrying — the client-driven
> bootstrap. The toolbar export button emits the **stdio** config
> (`command` = the distributed shim, `args` = `--stdio --hub-aumid <AUMID>`),
> falling back to the HTTP snippet only when there's no broker payload (dev).
> `.mcp.json` is now a stdio config pointing at the build-tree broker for
> contributors; `Install.ps1` prints the ready-to-paste per-user stdio snippet.
>
> Verified on a real packaged install: (1) the shim appears in
> `%LOCALAPPDATA%\ShaderLab\bin\` on GUI launch; (2) with a shim held open, a
> naive overwrite is blocked but the real rename-then-write succeeds and the held
> shim keeps running (update-immune); (3) with **no hub running**, the distributed
> shim activates the packaged hub, the GUI session then registers, and
> `use_session` + `graph_add_node` drive end-to-end through the render worker;
> broker smoke 26/26 and the HTTP suite 40/40 unregressed.

Note the shim IS `ShaderLabMcpBroker.exe --stdio` (one binary, two modes — Step 5);
"the shim" below means a copy of that exe placed on a stable unpackaged path.

- ShaderLab copies `ShaderLabMcpBroker.exe` to `%LOCALAPPDATA%\ShaderLab\bin\` on
  launch, using **rename-then-write**: overwrite-in-place is blocked while an old shim
  is running, but renaming the old file and writing a new one at the original path
  succeeds and leaves the running process untouched.
- The client config points at that stable path. Being unpackaged, the shim is immune to
  update and uninstall, and it can still activate the packaged hub AUMID (a
  non-packaged caller activating a packaged app is proven to work).
- `Install.ps1` prints the ready-to-paste client snippet. `.mcp.json` becomes a stdio
  config pointing at the build-tree shim for contributors.
- **Toolbar export button** (was: HTTP snippet) — **done**: emits the stdio config
  pointing at the distributed shim with `--hub-aumid` (see the ✅ note above).
  `ActivityCallback`'s `peerAddress → clientId` rename landed with Step 9. The richer
  `MCP: session 1 of 2` / `MCP: no hub` toggle label (a hub round-trip on the UI tick)
  was **not** done — it's the one intentionally-deferred UI nicety, tracked in the
  manual sweep's follow-up list, not a blocker.

**Upgrade behaviour to document.** A newer MSIX does *not* update the MCP the client is
talking to, and that is deliberate:

| Component | Auto-updated? | When it actually changes |
|---|---|---|
| Hub + sessions (packaged) | Yes, immediately | Next activation / app launch |
| Shim **binary on disk** | No — it's a copy | When ShaderLab next re-copies it |
| Shim **process** the client is running | No — immune by design | Only when the MCP client restarts |

Sequence for an in-place upgrade with a client connected: the install kills every
packaged process (sessions and hub) but not the shim, so the client's stdio server
stays alive instead of hitting EOF; `list_sessions` returns empty and `tools/call`
returns a clean "no session attached"; the shim re-activates the AUMID and gets the new
hub; the next ShaderLab launch registers a session and refreshes the on-disk shim. A
routine release therefore keeps a running client working. Only a deliberate protocol
break severs it, and then the old shim reports "no hub — restart your MCP client".

**Verify:** install version *n*, connect a client, install *n+1* in place, confirm the
client survives and recovers once ShaderLab relaunches.

> **Automated the mechanism, not the full MSIX-upgrade path.** The rename-then-write
> update-immunity (a held shim survives a re-distribution) and the client-driven hub
> activation are verified above. The literal install-*n* → install-*n+1*-with-client-
> connected sequence needs two signed builds and is in the end-of-migration
> **manual sweep** (below).

---

## Step 9 — Delete HTTP *(point of no return)* — ✅ done 2026-08-10

> **Done.** `McpRouter` lost the Winsock listener (`Start`/`Stop`/`Port`/`IsRunning`/
> `ListenerThread`/`HandleConnection`/`WSAStartup`/`WSACleanup`/`ws2_32.lib`/CORS)
> and the `GET /` health route; it keeps `AddRoute` / `RouteRequest` / `HasRoute` /
> `HasSpecificRoute` and fires `ActivityCallback` from the top-level `POST /` with a
> `clientId` (was `peerAddress`). `McpRouter.cpp` now compiles with the PCH (no more
> `NotUsing`). `MainWindow` and `ShaderLabHeadless` lost every HTTP call site
> (`Start(47808)` / `--serve` / `--port`); the GUI toggle + `~MainWindow` drive the
> session client only, and the activity indicator keys off `m_sessionClient`.
> `RunTests.ps1` was **ported to stdio in the same change**: it starts a shim, pins
> the first session, and runs every test as a `tools/call` (the one `GET /graph` became
> `graph_overview`). CI's headless step now pre-launches a hub + `--mcp-session` and
> drives via the shim. ABI **2 → 3**. Grep confirms `47808`/`WSA`/`ws2_32` appear
> nowhere outside `CHANGELOG.md` and the decision log; decisions #31 and #58 are marked
> superseded by #71. Verified per the status line above.

- Remove `Start` / `Stop` / `Port` / `ListenerThread` / `HandleConnection`, the sockets,
  `WSAStartup` / `WSACleanup`, `ws2_32.lib` and the CORS handling. Keep `AddRoute`,
  `RouteRequest`, `Response` and `ActivityCallback`.
- Delete `POST /` and `GET /` (~480 lines). Note that removing the catch-all is a
  **behaviour change for every unmatched path**, not just cleanup.
- Port `RunTests.ps1` to stdio in the same commit — it POSTs to `/mcp`, which resolves
  only via that catch-all.
- Bump the ABI. Rewrite [mcp-server.md](../hosts/mcp-server.md) (it claims 27 tools and
  port 47808; there are 40). Add a decision-log entry; update `README.md`,
  `docs/architecture/{overview,engine-host-split,threading-model}.md`,
  `.github/copilot-instructions.md` and `.context/resume.md`; mark decisions #31 and #58
  superseded.

**Verify:** full CI plus `RunBrokerSmoke.ps1` and `RunHeadlessSmoke.ps1`. Grepping for
`47808` and `WSA` should return nothing outside `CHANGELOG.md` and the decision log.

---

## Open risks

1. **ARM64 is never exercised at runtime.** `ci.yml` pins `platform: [x64]` and
   `release.yml` runs no tests, yet both spikes were measured only on ARM64. The
   coverage is exactly inverted from what you'd want.
2. **CI can only test the unpackaged identity path**, so `IApplicationActivationManager`
   — the mechanism that actually ships — stays untested in automation.
3. **The unpackaged fallback is the weak point of binary pairing** — the one path where
   "same build" is asserted rather than proven by the OS. Assert in the smoke test that
   an installed configuration refuses to use it.
4. **The documented unsigned install flow does not work for this app.** ShaderLab is a
   full-trust package (both `App` and the MCP `Hub` are `Windows.FullTrustApplication`),
   and per [Microsoft's unsigned-package rules](https://learn.microsoft.com/windows/msix/package/unsigned-package)
   an unsigned package with executable activations can only be installed **all-users
   (elevated)** — a per-user `Add-AppxPackage -AllowUnsigned` (what `Install.ps1` runs)
   fails `0x80073D2B` ("an unsigned package cannot include Executable activations").
   `Install.ps1` is non-elevated, so the README's "unsigned MSIX + Developer Mode + run
   `Install.ps1`" flow fails as written. Fix: either **sign the release** (a real cert →
   clean per-user install, no admin, no OID hack) or make `Install.ps1` **self-elevate**
   and document the admin requirement. Discovered running the install sweep on a real
   ARM64 box, 2026-08-10.

---

## Manual verification sweep (run once the migration is complete)

CI + the unit / broker / headless suites cover the transport, crypto, election, pairing,
and engine-route paths. This sweep covers only what automation can't: the packaged app
driven **the way a real user drives it** — install, launch, and a real MCP client over
stdio, on real hardware. Run it on a real machine (ideally both x64 and ARM64 — see
risk #1) before calling the migration signed-off.

The rule for this sweep: **do exactly what an end user does, and nothing else.** The only
terminal step is the install (`Install.ps1`, the shipped installer). Everything after is
driven from ShaderLab's UI and your MCP client, and every check is something the user can
*see* — no PowerShell probes, no activation helper, no log scraping. Two checks were
deliberately dropped as *not* end-user actions: the isolated `IApplicationActivationManager`
probe (the client-connect step below exercises the real `ActivateHub()` instead — a
terminal re-implementation could pass while the shipped path is broken), and the
rogue-unpackaged-session refusal (owned by the pairing unit tests + broker smoke). If a
check fails and you need to isolate it, the hub/shim/session log is at
`%LOCALAPPDATA%\ShaderLab\logs\broker-*.log`.

Prerequisites: a release build's signed-namespace MSIX zip (`release.yml` produces it),
Developer Mode on, and a real MCP client — the one the export snippet targets.

**Install & launch (Steps 5, 8)**
- [ ] `Install.ps1` on the release zip installs and ShaderLab launches from the Start
      menu, with **no** second Start-menu entry for the hub (it ships hidden). ⚠️ Run
      `Install.ps1` from an **elevated** PowerShell — ShaderLab is a full-trust app, and an
      unsigned package with executable activations can only be installed all-users (admin);
      a non-elevated `-AllowUnsigned` fails `0x80073D2B` (see risk #4).
- [ ] With MCP enabled, the toolbar shows a session/hub-ready state and the **export**
      button copies a working **stdio** config snippet.

**Real MCP client end-to-end — this one flow proves activation (Steps 6–9)**
- [ ] Paste the export snippet into your MCP client and connect. The client's shim
      activates the packaged hub on its own — no manual step. The session shows up in the
      client; list + select it and drive **every tool** at least once, including the image
      tools (`graph_snapshot` / `render_capture_node` inline) and one large (~4K) inline
      capture near the 64 MB frame cap, with the image rendering correctly in the client.
- [ ] **Warm-hub survival:** fully close the MCP client, then reopen and reconnect. It
      reconnects **without** a cold ShaderLab restart — proof the hub outlived the client's
      job object instead of dying with it.
- [ ] **Update-immune shim:** with the client connected and working, relaunch ShaderLab —
      and, separately, install *n → n+1* (the real in-place upgrade). Neither disrupts the
      connected client; it recovers once ShaderLab is back.

**Two windows & live UI (Step 7)**
- [ ] Open **two ShaderLab windows**, MCP on in both; each registers its own session. In
      the client the two sessions are distinguishable; select one and mutate its graph —
      the change lands in **that** window on screen, not the other.
- [ ] **GPU switch mid-request:** start a slow tool call from the client, then switch GPUs
      from ShaderLab's **GPU dropdown**. The in-flight call returns a clean error (not a
      hang) and the app keeps working.
- [ ] **Graceful close:** close a window with its **X** while the client is connected. It
      closes promptly — no ~30 s stall — the session disappears from the client's list, and
      the client sees a clean `session_gone`, not a dropped connection.

---

Back to [docs/](../README.md) • [Repo root](../../README.md)
