---
name: shaderlab-run
description: Deploy, launch, and drive the packaged ShaderLab app — MSIX registration, shell activation, enabling the MCP session, and running the MCP test suite against a live GUI or a headless session. Use when asked to run/launch/deploy ShaderLab, to see a change in the real app, to connect over MCP, or when an MCP session is missing or stale.
---

# Running ShaderLab

Packaged WinUI 3 app. Deploy has several traps that fail in ways that look like app
bugs. Follow the order below.

Prefer **headless** when the question is numeric — it skips all of this. See
`docs/hosts/headless.md`, and `CLAUDE.md` for the inner-loop table.

## 0. Build first

Use the **shaderlab-build** skill. The GUI locks `ShaderLab.exe` and
`ShaderLabEngine.dll` in the layout, and a lingering `ShaderLabMcpBroker` hub locks
the layout's broker copy — **kill both before rebuilding** or the copy step fails.

```pwsh
Get-Process ShaderLab, ShaderLabMcpBroker -ErrorAction SilentlyContinue | Stop-Process -Force
```

A titleless lingering `ShaderLab.exe` is a hung shutdown — safe to kill.

## 1. Register the package — from the layout root, never `AppX\`

```pwsh
Add-AppxPackage -Register <Platform>\Debug\ShaderLab\AppxManifest.xml   # x64 or ARM64
```

**Why the layout root matters:** an incremental build refreshes the layout root but
**not** `AppX\`. A registration pointing at `...\ShaderLab\AppX` runs stale binaries,
and new-exe/old-resources mixes abort at startup — which reads as an app crash, not a
deploy problem.

Binary-only rebuilds need no re-register. When registration fails:

| HRESULT | Meaning | Fix |
|---|---|---|
| `0x80073D02` | Package in use | Kill ShaderLab + broker processes, retry |
| `0x80073CFB` | Manifest content changed, version didn't | `Get-AppxPackage -Name ShaderLab \| Remove-AppxPackage`, then re-register. Config in `%LOCALAPPDATA%\ShaderLab` survives |
| `0x80070490` | Stale registration ("indexed state handler") | `Remove-AppxPackage` then re-register |
| — | Registration silently points at the old path | Re-registering the *same version* is a no-op that keeps the old path. `Remove-AppxPackage` first |

## 2. Launch by shell activation — never the exe directly

```pwsh
explorer.exe "shell:AppsFolder\ShaderLab_9v3yd384n9j18!App"
```

`Start-Process ShaderLab.exe` crashes with a `Debug Error! abort() has been called`
CRT dialog (packaged-app dependency resolution). The AUMIDs are
`ShaderLab_9v3yd384n9j18!App` and `…!Hub`.

**Cold Debug start can take >45 s.** There is no HTTP port to poll — readiness means
the session appears in `list_sessions`.

## 3. MCP

MCP is enabled by `%LOCALAPPDATA%\ShaderLab\config.json` containing `{"mcp": true}`;
alternatives are the `--mcp` arg or the toolbar toggle. On launch the GUI copies the
shim to
`%LOCALAPPDATA%\ShaderLab\bin\ShaderLabMcpBroker.exe` and registers a session with the
hub, which the shim activates on demand.

Transport is **shim → hub → session** over named pipes, bodies sealed end-to-end
(P-256 / HKDF / AES-GCM). There is no HTTP listener — it was deleted in stdio-migration
Step 9 (engine ABI 3).

To use it from this session: `list_sessions`, then `use_session <id>` to pin a window.

> **Note:** repo `.mcp.json` points the ShaderLab MCP server at
> `x64/Debug/ShaderLabMcpBroker/ShaderLabMcpBroker.exe` — the right default, since x64
> is what CI and most contributors build. **On an ARM64 host that path is wrong**: it
> either does not exist or is a stale x64 build running under emulation, while your
> real shim is in `ARM64\Debug\`. `.mcp.json` is committed and has no per-platform
> form, so override it locally rather than editing it — and if MCP behaves oddly,
> check which binary is actually running before debugging the protocol.

## 4. MCP test suite

Against the running GUI (the shim activates the packaged hub):

```pwsh
pwsh -NoProfile -File .\Tests\RunTests.ps1 -HubAumid 'ShaderLab_9v3yd384n9j18!Hub'
```

No-GUI, the way CI does it (GUI-only tests self-skip):

```pwsh
$env:SHADERLAB_MCP_ALLOW_UNPACKAGED = '1'
$bin  = '.\x64\Debug'   # or .\ARM64\Debug
$pipe = "ShaderLab.mcp.dev.$([guid]::NewGuid().ToString('N'))"
$hub  = Start-Process $bin\ShaderLabMcpBroker\ShaderLabMcpBroker.exe `
          -ArgumentList '--hub','--pipe',$pipe,'--idle-exit-sec','600' -PassThru
$sess = Start-Process $bin\ShaderLabHeadless\ShaderLabHeadless.exe `
          -ArgumentList '--graph','Tests\fixtures\test_cli_basic.json','--mcp-session',
                        '--pipe',$pipe,'--session-label','dev','--adapter','warp' -PassThru
try   { pwsh -NoProfile -File .\Tests\RunTests.ps1 -Pipe $pipe -Adapter warp }
finally { Stop-Process -Id $sess.Id, $hub.Id -Force -ErrorAction SilentlyContinue }
```

Verifying **hub activation** specifically: the acceptance check is a real MCP client
connecting — its shim runs the shipped `ActivateHub()` path. Close and reopen the
client; it should reconnect with no cold ShaderLab restart (the hub outlives the
client's job object, which is why `IApplicationActivationManager` is used over
`CreateProcess`). A PowerShell/C# re-implementation of `ActivateHub` tests the OS API
rather than the shim and **can false-pass** — don't use it as the gate.

## Screenshots and visual confirmation

Capture through the graph rather than the screen: `render_capture_node` over MCP, or
headless `--output`. That captures the actual scRGB FP16 pipeline output; an OS
screenshot of an HDR window does not.

**But a captured PNG is 8-bit SDR**, so it has clipped everything above scRGB 1.0
(80 nits) and lost wide-gamut negatives — it cannot settle an HDR or gamut question.
Prefer numbers (`read_pixel_region`, `read_analysis_output`) and, when you need to
*look*, capture a diagnostic node whose output is SDR by construction — `Nit Map`,
`Luminance Heatmap`, `Gamut Highlight`, `CIE Chromaticity Plot`, `Delta E Comparator`
in Heatmap mode. Say which you used, and flag when a verdict is passing through a tone
map. Full rationale in `CLAUDE.md` §*Looking at HDR output*.
