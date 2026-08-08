# MCP Migration: HTTP → stdio + broker relay

Implementation plan for replacing the embedded HTTP MCP server with a stdio transport
fronted by a singleton broker. Written to be picked up on a different machine — see
[Picking this up](#picking-this-up) for prerequisites and the exact commands.

Status: **planning complete, implementation not started.** Steps 1–9 below are
outstanding. Two throwaway spikes have already settled the platform questions; their
results are recorded in [Settled by spike](#settled-by-spike) so they are not
re-litigated.

---

## Why

The MCP server is a Winsock HTTP listener embedded in every host
(`Engine/Mcp/McpHttpServer.cpp`), default port 47808.

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
marked ⚠ are worth re-checking if they ever look wrong, and none of this was verified
at the manifest's declared `10.0.17763` floor.

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

**Not started:** Steps 1–9.

---

## Picking this up

### Prerequisites

Standard repo prerequisites from [build.md](build.md), plus:

- The MCP suite needs a **running GUI ShaderLab** — it drives `tools/call`, which is
  GUI-only until Step 3. It does not build or launch anything itself.
- Deploy from the per-arch layout, never from `AppX\`:
  `Add-AppxPackage -Register <Platform>\<Config>\ShaderLab\AppxManifest.xml`

### Verification commands

```pwsh
# Unit suite (no GPU dependency beyond WARP)
<Platform>\<Config>\ShaderLabTests\ShaderLabTests.exe --adapter warp

# MCP regression suite — requires ShaderLab already running
pwsh -NoProfile -File .\Tests\RunTests.ps1

# Headless smoke
.\Tests\RunHeadlessSmoke.ps1 -Configuration Debug -Platform x64
```

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

## Step 1 — Route hygiene *(HTTP still live)*

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

---

## Step 2 — Transport-neutral types + rename *(one-way)*

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

---

## Step 3 — MCP dispatcher + tool catalog into the engine

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

---

## Step 4 — Frame codec, crypto, peer identity *(no IPC yet)*

New `Engine/Mcp/McpFrame.{h,cpp}`, `McpCrypto.{h,cpp}`, `McpPeerIdentity.{h,cpp}`

- Frames: 4-byte little-endian length + UTF-8 payload. The header carries
  `{channelId, seq}` in the clear (the hub legitimately routes on it); the body is
  sealed. Keep that split explicit in the type so it cannot drift.
- 64 MB cap. A 4K inline capture is ~33 MB of base64; 8K would be ~130 MB, so either
  cap resolution server-side or fail explicitly rather than desyncing.
- Crypto: BCrypt ephemeral **P-256**. X25519 was rejected — CNG named-curve support is
  unverified at the declared `10.0.17763` floor and it buys nothing against an empty
  threat model. Two traps: `BCryptDeriveKey` with `BCRYPT_KDF_RAW_SECRET` returns the
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

---

## Step 5 — Hub + shim, zero sessions

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

---

## Step 6 — Session client + headless session

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
  `MainWindow` member and a headless session cannot serve it.

**Verify:** the full `RunBrokerSmoke.ps1` in CI on WARP, end-to-end through real engine
routes. This retires election, framing, crypto and reconnect risk. It does **not**
retire GUI integration risk: `HeadlessSink::Dispatch` is a direct synchronous call with
no DispatcherQueue, no render dispatcher, no XAML, and all 8 event hooks are no-ops.

---

## Step 7 — GUI session client

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

**Verify:** manual — two ShaderLab windows, session routing via `use_session`, a GPU
switch mid-request, clean shutdown, and every tool exercised.

---

## Step 8 — Shim distribution

`MainWindow` startup, `scripts/Install.ps1`, `.mcp.json`

- ShaderLab copies `ShaderLabMcpShim.exe` to `%LOCALAPPDATA%\ShaderLab\bin\` on launch,
  using **rename-then-write**: overwrite-in-place is blocked while an old shim is
  running, but renaming the old file and writing a new one at the original path
  succeeds and leaves the running process untouched.
- The client config points at that stable path. Being unpackaged, the shim is immune to
  update and uninstall, and it can still activate the packaged hub AUMID (a
  non-packaged caller activating a packaged app is proven to work).
- `Install.ps1` prints the ready-to-paste client snippet. `.mcp.json` becomes a stdio
  config pointing at the build-tree shim for contributors.

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

---

## Step 9 — Delete HTTP *(point of no return)*

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

---

Back to [docs/](../README.md) • [Repo root](../../README.md)
