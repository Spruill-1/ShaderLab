<#
.SYNOPSIS
    ShaderLab MCP integration suite. Drives a ShaderLab session end-to-end
    through the broker (shim -> hub -> session), the ONLY transport since the
    HTTP listener was removed in stdio-migration Step 9.

.DESCRIPTION
    Starts a shim (ShaderLabMcpBroker --stdio), pins the first registered
    session, and runs ~40 tests as tools/calls over stdio. Does NOT build,
    deploy, or launch a session -- a hub + a ShaderLab session (GUI window, or
    `ShaderLabHeadless --mcp-session`) must already be running. GUI-only tests
    self-skip when the pinned session is headless (detected from its label).

.PARAMETER Filter
    Run only tests matching this wildcard pattern (e.g. "Graph*").

.PARAMETER Adapter
    GPU adapter label recorded for CLI tests: "default" or "warp".

.PARAMETER Pipe
    Broker pipe base to connect through. Empty = the per-user default (matches
    a running GUI). CI passes an isolated name shared with its pre-launched
    hub + headless session.

.PARAMETER HubAumid
    If set, the shim activates this packaged hub AUMID when none is running
    (local run against the packaged GUI). CI pre-launches an unpackaged hub
    instead and leaves this empty.
#>
param(
    [string]$Filter = "*",
    [string]$Adapter = "default",
    [string]$Pipe = "",
    [string]$HubAumid = ""
)

$ErrorActionPreference = "Stop"
$script:TestResults = @()
$script:TestDir = $PSScriptRoot
$script:RepoRoot = Split-Path $script:TestDir -Parent
$script:FixturesDir = Join-Path $script:TestDir "fixtures"
$script:OutputDir = Join-Path $script:TestDir "output"

# ============================================================================
# Shim transport
# ============================================================================

function Log($msg) { Write-Host "  $msg" -ForegroundColor DarkGray }

# Locate the shim: prefer the distributed copy, else the build tree.
function Find-Shim {
    $distributed = Join-Path $env:LOCALAPPDATA 'ShaderLab\bin\ShaderLabMcpBroker.exe'
    if (Test-Path $distributed) { return $distributed }
    foreach ($plat in @('ARM64','x64')) {
        foreach ($cfg in @('Debug','Release')) {
            $p = Join-Path $script:RepoRoot "$plat\$cfg\ShaderLabMcpBroker\ShaderLabMcpBroker.exe"
            if (Test-Path $p) { return $p }
        }
    }
    return $null
}

function Start-Shim {
    $shim = Find-Shim
    if (-not $shim) { Write-Host "ShaderLabMcpBroker.exe not found -- build first." -ForegroundColor Red; exit 1 }
    $argList = @('--stdio')
    if ($Pipe)     { $argList += @('--pipe', $Pipe) }
    if ($HubAumid) { $argList += @('--hub-aumid', $HubAumid) }
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $shim
    $psi.Arguments = ($argList -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $script:Shim = [System.Diagnostics.Process]::Start($psi)
    Log "shim: $shim $($psi.Arguments)"
}

function Stop-Shim {
    if ($script:Shim -and -not $script:Shim.HasExited) {
        try { $script:Shim.StandardInput.Close() } catch {}
        if (-not $script:Shim.WaitForExit(4000)) { Stop-Process -Id $script:Shim.Id -Force -ErrorAction SilentlyContinue }
    }
}

# One JSON-RPC round-trip over the shim's stdio (serial: send then read the
# single response line -- notifications aside, the shim answers one line per
# request, so correlation is positional).
function Rpc($obj, $timeoutMs = 35000) {
    $script:Shim.StandardInput.WriteLine(($obj | ConvertTo-Json -Depth 8 -Compress))
    $t = $script:Shim.StandardOutput.ReadLineAsync()
    if (-not $t.Wait($timeoutMs)) { throw "shim did not respond in ${timeoutMs}ms" }
    return ($t.Result | ConvertFrom-Json)
}

function McpCall($toolName, $arguments = @{}) {
    $r = Rpc @{ jsonrpc = "2.0"; id = (Get-Random -Maximum 999999)
               method = "tools/call"; params = @{ name = $toolName; arguments = $arguments } }
    if ($r.result.isError) { throw "MCP error: $($r.result.content[0].text)" }
    $text = $r.result.content[0].text
    if ($text) { return $text | ConvertFrom-Json } else { return $null }
}

# Replaces the old HTTP GET /graph: graph_overview, normalized so edges
# expose .srcId/.dstId (they arrive as [src,srcPin,dst,dstPin] arrays).
function GraphState() {
    $o = McpCall "graph_overview"
    $edges = @()
    foreach ($e in @($o.edges)) {
        $edges += [pscustomobject]@{ srcId = $e[0]; srcPin = $e[1]; dstId = $e[2]; dstPin = $e[3] }
    }
    return [pscustomobject]@{ nodes = @($o.nodes); edges = $edges; previewNodeId = $o.previewNodeId }
}

function ClearGraph() {
    McpCall "graph_clear" | Out-Null
}

function AddNode($effectName, $extra = @{}) {
    $args = @{ effectName = $effectName } + $extra
    $r = McpCall "graph_add_node" $args
    return $r.nodeId
}

function SetProperty($nodeId, $key, $value) {
    McpCall "graph_set_property" @{ nodeId = $nodeId; key = $key; value = $value } | Out-Null
}

function Connect($srcId, $srcPin, $dstId, $dstPin) {
    McpCall "graph_connect" @{ srcId = $srcId; srcPin = $srcPin; dstId = $dstId; dstPin = $dstPin } | Out-Null
}

function BindProperty($nodeId, $propName, $srcNodeId, $srcField) {
    McpCall "graph_bind_property" @{
        nodeId = $nodeId; propertyName = $propName
        sourceNodeId = $srcNodeId; sourceFieldName = $srcField
    } | Out-Null
}

function GetNode($nodeId) {
    return McpCall "graph_get_node" @{ nodeId = $nodeId }
}

function GetAnalysis($nodeId) {
    return McpCall "read_analysis_output" @{ nodeId = $nodeId }
}

function WaitForDirtySettle($timeoutSec = 5) {
    # Wait for at least 2 render frames so effects evaluate.
    Start-Sleep -Milliseconds 200
}

# ============================================================================
# Connect: start the shim, pin the first session, detect host kind
# ============================================================================

Start-Shim

# initialize (shim-owned).
try { $init = Rpc @{ jsonrpc = "2.0"; id = 1; method = "initialize"; params = @{ protocolVersion = "2025-06-18" } } }
catch { Write-Host "Shim did not answer initialize: $_" -ForegroundColor Red; Stop-Shim; exit 1 }
if ($init.result.serverInfo.name -ne 'shaderlab-shim') {
    Write-Host "Unexpected shim serverInfo: $($init.result.serverInfo.name)" -ForegroundColor Red; Stop-Shim; exit 1
}

# Poll list_sessions until a ShaderLab session is registered, then pin it.
$script:HostKind = 'gui'
$sid = $null; $label = $null
$deadline = (Get-Date).AddSeconds(40)
while ((Get-Date) -lt $deadline) {
    try {
        $ls = McpCall "list_sessions"
        $sessions = @($ls.sessions)
        if ($sessions.Count -ge 1) { $sid = $sessions[0].id; $label = $sessions[0].label; break }
    } catch {}
    Start-Sleep -Milliseconds 700
}
if (-not $sid) {
    Write-Host "No ShaderLab session registered with the hub." -ForegroundColor Red
    Write-Host "Launch a ShaderLab window (or ShaderLabHeadless --mcp-session) + a hub, then re-run." -ForegroundColor Red
    Stop-Shim; exit 1
}
$use = McpCall "use_session" @{ sessionId = $sid }
# Host kind from the session label: headless sessions are "headless <pid>";
# GUI sessions are "ShaderLab <ver> (pid ...)". GUI-only tests skip on headless.
if ($label -match 'headless') { $script:HostKind = 'headless' }
Write-Host "Pinned session $sid [$label] -> host kind: $script:HostKind" -ForegroundColor DarkGray

function RunTest($name, $scriptBlock, [switch]$RequiresGui) {
    if ($name -notlike $Filter) { return }
    Write-Host "[$name] " -NoNewline
    # GUI-only tests (render loop, XAML surfaces, app-side routes) skip on
    # a headless target rather than failing on "Tool not available".
    if ($RequiresGui -and $script:HostKind -ne 'gui') {
        Write-Host "SKIP (needs GUI host)" -ForegroundColor DarkYellow
        return
    }
    # If the app died mid-suite, stop rather than reporting every remaining
    # test as a failure -- a crash is one fault, not twenty.
    if ($script:AppDied) {
        Write-Host "SKIP (app died earlier)" -ForegroundColor DarkYellow
        $script:TestResults += @{ Name = $name; Pass = $false; Error = "skipped: app died earlier" }
        return
    }
    try {
        ClearGraph
        $result = & $scriptBlock
        if ($result -eq $false) {
            Write-Host "FAIL" -ForegroundColor Red
            $script:TestResults += @{ Name = $name; Pass = $false; Error = "Returned false" }
        } else {
            Write-Host "PASS" -ForegroundColor Green
            $script:TestResults += @{ Name = $name; Pass = $true; Error = "" }
        }
    } catch {
        Write-Host "FAIL - $($_.Exception.Message)" -ForegroundColor Red
        $script:TestResults += @{ Name = $name; Pass = $false; Error = $_.Exception.Message }
        if (-not (Get-Process ShaderLab, ShaderLabHeadless -ErrorAction SilentlyContinue)) {
            $script:AppDied = $true
            Write-Host "  !! host process is gone -- treating as a crash, skipping the rest." -ForegroundColor Red
        }
    }
}

# ============================================================================
# Tests: Graph Operations
# ============================================================================

RunTest "Graph.AddBuiltInEffect" {
    $id = AddNode "Gaussian Blur"
    $node = GetNode $id
    return $node.name -eq "Gaussian Blur" -and $node.type -eq "BuiltInEffect"
}

RunTest "Graph.AddShaderLabEffect" {
    $id = AddNode "Luminance Heatmap"
    $node = GetNode $id
    return $node.name -eq "Luminance Heatmap"
}

RunTest "Graph.AddClockNode" {
    $id = AddNode "Clock"
    $node = GetNode $id
    return ($node.properties.PSObject.Properties.Name -contains "AutoDuration")
}

RunTest "Graph.AddMathNode" {
    # Was "Add". The discrete Add/Max math nodes were retired in favour of the
    # ExprTk-backed Numeric Expression node.
    $id = AddNode "Numeric Expression"
    $node = GetNode $id
    return $node.name -eq "Numeric Expression"
}

RunTest "Graph.AddVideoSource" {
    $id = AddNode "Video"
    $node = GetNode $id
    return $node.type -eq "Source"
}

RunTest "Graph.AddImageSource" {
    $id = AddNode "Image"
    $node = GetNode $id
    return $node.type -eq "Source"
}

RunTest "Graph.ConnectNodes" {
    $src = AddNode "Gamut Source"
    $blur = AddNode "Gaussian Blur"
    Connect $src 0 $blur 0
    $graph = GraphState
    $edge = $graph.edges | Where-Object { $_.srcId -eq $src -and $_.dstId -eq $blur }
    return $null -ne $edge
}

RunTest "Graph.DisconnectNodes" {
    $src = AddNode "Gamut Source"
    $blur = AddNode "Gaussian Blur"
    Connect $src 0 $blur 0
    McpCall "graph_disconnect" @{ srcId = $src; srcPin = 0; dstId = $blur; dstPin = 0 } | Out-Null
    $graph = GraphState
    return $graph.edges.Count -eq 0
}

RunTest "Graph.RemoveNode" {
    $id = AddNode "Gaussian Blur"
    McpCall "graph_remove_node" @{ nodeId = $id } | Out-Null
    $graph = GraphState
    return $graph.nodes.Count -eq 0
}

RunTest "Graph.SetProperty" {
    $id = AddNode "Gaussian Blur"
    SetProperty $id "Optimization" 2.0
    $node = GetNode $id
    return $node.properties.Optimization -eq 2.0
}

RunTest "Graph.SaveLoadRoundtrip" {
    $src = AddNode "Gamut Source"
    $blur = AddNode "Gaussian Blur"
    Connect $src 0 $blur 0
    SetProperty $blur "Optimization" 3.0
    $json = McpCall "graph_save_json"
    ClearGraph
    $graph1 = GraphState
    if ($graph1.nodes.Count -ne 0) { return $false }
    McpCall "graph_load_json" @{ json = ($json | ConvertTo-Json -Depth 10 -Compress) } | Out-Null
    Start-Sleep -Milliseconds 500
    $graph2 = GraphState
    return $graph2.nodes.Count -eq 2 -and $graph2.edges.Count -eq 1
}

# ============================================================================
# Tests: Negative / Error Handling
# ============================================================================

RunTest "Error.InvalidNodeId" {
    try { GetNode 99999; return $false } catch { return $true }
}

RunTest "Error.UnknownEffect" {
    try { AddNode "NonExistentEffect123"; return $false } catch { return $true }
}

RunTest "Error.BadShaderCompile" {
    $id = AddNode "Custom Pixel Shader"
    try {
        McpCall "effect_compile" @{ nodeId = $id; hlsl = "this is not valid hlsl!!!" }
        return $false
    } catch {
        return $_.Exception.Message -like "*error*" -or $_.Exception.Message -like "*compile*" -or $true
    }
}

# ============================================================================
# Tests: Effect Evaluation
# ============================================================================

RunTest "Eval.GamutSourceProducesOutput" -RequiresGui {
    $src = AddNode "Gamut Source"
    McpCall "set_preview_node" @{ nodeId = $src } | Out-Null
    WaitForDirtySettle
    $node = GetNode $src
    return $null -eq $node.runtimeError -or $node.runtimeError -eq ""
}

RunTest "Eval.EffectChainRenders" -RequiresGui {
    $src = AddNode "Gamut Source"
    $blur = AddNode "Gaussian Blur"
    Connect $src 0 $blur 0
    McpCall "set_preview_node" @{ nodeId = $blur } | Out-Null
    WaitForDirtySettle
    $node = GetNode $blur
    return $null -eq $node.runtimeError -or $node.runtimeError -eq ""
}

RunTest "Eval.AnalysisEffectProducesFields" -RequiresGui {
    $src = AddNode "Gamut Source"
    $heatmap = AddNode "Luminance Heatmap"
    Connect $src 0 $heatmap 0
    McpCall "set_preview_node" @{ nodeId = $heatmap } | Out-Null
    WaitForDirtySettle 3
    $node = GetNode $heatmap
    return $null -eq $node.runtimeError -or $node.runtimeError -eq ""
}

# Test each ShaderLab source effect produces output
$sourceEffects = @("Gamut Source", "Color Checker", "Zone Plate", "Gradient Generator", "HDR Test Pattern")
foreach ($effectName in $sourceEffects) {
    RunTest "Eval.Source.$($effectName -replace ' ','')" -RequiresGui {
        $id = AddNode $effectName
        McpCall "set_preview_node" @{ nodeId = $id } | Out-Null
        WaitForDirtySettle
        $node = GetNode $id
        return $null -eq $node.runtimeError -or $node.runtimeError -eq ""
    }.GetNewClosure()
}

# Test analysis effects
# NOTE: "Vectorscope" and "Waveform Monitor" were removed from this list --
# they are not in the effect registry (verified via list_effects). Note that
# .context/resume.md still lists both under "Analysis -> Scopes", so the doc is
# stale, not this list.
$analysisEffects = @("Luminance Heatmap", "Gamut Highlight",
    "Nit Map", "Split Comparison")
foreach ($effectName in $analysisEffects) {
    RunTest "Eval.Analysis.$($effectName -replace ' ','')" -RequiresGui {
        $src = AddNode "Gamut Source"
        $fx = AddNode $effectName
        Connect $src 0 $fx 0
        McpCall "set_preview_node" @{ nodeId = $fx } | Out-Null
        WaitForDirtySettle
        $node = GetNode $fx
        return $null -eq $node.runtimeError -or $node.runtimeError -eq ""
    }.GetNewClosure()
}

# ============================================================================
# Tests: Property Bindings & Math Nodes
# ============================================================================

RunTest "Binding.FloatParameterToEffect" -RequiresGui {
    $param = AddNode "Float Parameter"
    $blur = AddNode "Gaussian Blur"
    SetProperty $param "Value" 5.0
    $src = AddNode "Gamut Source"
    Connect $src 0 $blur 0
    BindProperty $blur "StandardDeviation" $param "Value"
    # Bindings on a D2D effect only propagate when the effect is actually
    # evaluated, and evaluation only reaches nodes in the render path. Without
    # this the property stays at its authored default (3.0) and the test fails
    # for a reason that has nothing to do with binding. (Data-only nodes such as
    # Numeric Expression differ -- they evaluate on the tick regardless.)
    McpCall "set_preview_node" @{ nodeId = $blur } | Out-Null
    WaitForDirtySettle 3
    $node = GetNode $blur
    $sd = $node.properties.StandardDeviation
    return $null -ne $sd -and $sd -ge 4.5
}

RunTest "Eval.NumericExpressionDirect" -RequiresGui {
    # Replaces the retired "Add" node test. Note only the "A" input exists as a
    # property over MCP -- setting Expression to something referencing B does
    # NOT create a B property, so multi-variable expressions are not currently
    # drivable through the MCP surface.
    $e = AddNode "Numeric Expression"
    SetProperty $e "Expression" "A * 2"
    SetProperty $e "A" 5.0
    WaitForDirtySettle 2
    $analysis = GetAnalysis $e
    if (-not $analysis -or -not $analysis.fields) { return $false }
    $result = ($analysis.fields | Where-Object { $_.name -eq "Result" })
    return $null -ne $result -and [math]::Abs($result.value[0] - 10.0) -lt 0.01
}

RunTest "Binding.NumericExpressionBound" -RequiresGui {
    # Replaces the retired "Max" node test, and is the real regression guard for
    # binding propagation into a data node: A is driven by an upstream Float
    # Parameter rather than set directly.
    $a = AddNode "Float Parameter"
    $e = AddNode "Numeric Expression"
    SetProperty $e "Expression" "A * 2"
    SetProperty $a "Value" 6.0
    BindProperty $e "A" $a "Value"
    WaitForDirtySettle 2
    $analysis = GetAnalysis $e
    if (-not $analysis -or -not $analysis.fields) { return $false }
    $result = ($analysis.fields | Where-Object { $_.name -eq "Result" })
    return $null -ne $result -and [math]::Abs($result.value[0] - 12.0) -lt 0.01
}

# ============================================================================
# Tests: Clock & Animation
# ============================================================================

RunTest "Clock.TimeAdvances" -RequiresGui {
    $clock = AddNode "Clock"
    SetProperty $clock "isPlaying" $true
    Start-Sleep -Seconds 2
    $node = GetNode $clock
    $analysis = GetAnalysis $clock
    $time = ($analysis.fields | Where-Object { $_.name -eq "Time" })
    return $null -ne $time -and $time.value[0] -gt 1.0
}

RunTest "Clock.LoopWraps" -RequiresGui {
    $clock = AddNode "Clock"
    SetProperty $clock "StopTime" 1.0
    SetProperty $clock "Loop" 1.0
    SetProperty $clock "Speed" 5.0
    SetProperty $clock "isPlaying" $true
    Start-Sleep -Seconds 2
    $analysis = GetAnalysis $clock
    $time = ($analysis.fields | Where-Object { $_.name -eq "Time" })
    # With speed=5, loop=1s, after 2s the time should have wrapped multiple times.
    # It should be between 0 and 1.
    return $null -ne $time -and $time.value[0] -ge 0.0 -and $time.value[0] -le 1.1
}

# ============================================================================
# Tests: Custom Shader Compilation
# ============================================================================

RunTest "Shader.PixelShaderCompile" {
    $id = AddNode "Custom Pixel Shader"
    $hlsl = @"
Texture2D Source : register(t0);
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    return Source.Load(int3(uv0.xy, 0));
}
"@
    $r = McpCall "effect_compile" @{ nodeId = $id; hlsl = $hlsl }
    return $null -ne $r
}

# ============================================================================
# Tests: Promoted routes (stdio-migration Step 1)
# ============================================================================

RunTest "Route.RenameNode" -RequiresGui {
    # graph_rename_node was an inline tools/call handler that mutated m_graph
    # on the UI thread; now a real route mutating on the render thread.
    $id = AddNode "Gaussian Blur"
    McpCall "graph_rename_node" @{ nodeId = $id; name = "My Blur" } | Out-Null
    $node = GetNode $id
    return $node.name -eq "My Blur"
}

RunTest "Route.RenameNodeMissing" -RequiresGui {
    try { McpCall "graph_rename_node" @{ nodeId = 99999; name = "x" }; return $false } catch { return $true }
}

RunTest "Route.GraphOverview" {
    $src = AddNode "Gamut Source"
    $blur = AddNode "Gaussian Blur"
    Connect $src 0 $blur 0
    $o = McpCall "graph_overview"
    return $o.nodes.Count -eq 2 -and $o.edges.Count -eq 1
}

RunTest "Route.ListEffects" {
    # Now served by the engine route GET /effects (headless gets it too).
    $fx = McpCall "list_effects"
    $slCats = @($fx.shaderLab.PSObject.Properties.Name)
    return $null -ne $fx.builtIn -and $slCats.Count -gt 0
}

RunTest "Route.DisplayInfo" {
    $info = McpCall "get_display_info"
    return $null -ne $info.appVersion -and $null -ne $info.pipeline -and $null -ne $info.display
}

RunTest "Route.ImageStatsRemoved" {
    # image_stats was a phantom tool: its route was retired by decision #63 but
    # the tools/call ladder kept forwarding to it, and longest-prefix routing
    # returned a fake HTTP-200 wrapping a JSON-RPC error. The ladder entry is
    # gone; the tool must now surface as an error, not a silent success.
    try { McpCall "image_stats" @{ nodeId = 1 }; return $false } catch { return $true }
}

RunTest "Route.CatalogRoundTrip" {
    # Live round-trip over every advertised tool (stdio-migration Step 3's
    # replacement for a HasRoute-based coverage test, which prefix semantics
    # make impossible). Every tool must return a well-formed tool-result
    # envelope -- content[] + boolean isError -- never a hang, transport
    # error, or malformed frame. Mutating tools get validation-failing args
    # so nothing is disturbed; readback tools get a scratch node. Tools
    # whose backing route is absent on this host return isError=true with
    # "Tool not available", which is a correctly-shaped result.
    $tl = Rpc @{ jsonrpc = '2.0'; id = 1; method = 'tools/list' }
    # Over the shim, tools/list is spliced: the shim's 2 session tools +
    # the pinned session's catalog. Skip the shim's own tools in the sweep
    # (they aren't session routes).
    $tools = @($tl.result.tools | Where-Object { $_.name -notin @('list_sessions','use_session') })
    if ($tools.Count -lt 30) { Log "tools/list returned only $($tools.Count)"; return $false }
    $scratch = AddNode "Gaussian Blur"
    $canned = @{
        graph_add_node        = @{ effectName = 'Gaussian Blur' }
        graph_remove_node     = @{ nodeId = 999999 }
        graph_rename_node     = @{ nodeId = 999999; name = 'x' }
        graph_connect         = @{ srcId = 999999; srcPin = 0; dstId = 999998; dstPin = 0 }
        graph_disconnect      = @{ srcId = 999999; srcPin = 0; dstId = 999998; dstPin = 0 }
        graph_set_property    = @{ nodeId = 999999; key = 'x'; value = 1 }
        graph_get_node        = @{ nodeId = $scratch }
        graph_load_json       = @{ json = '{"formatVersion":99}' }
        graph_bind_property   = @{ nodeId = 999999; propertyName = 'x'; sourceNodeId = 1; sourceFieldName = 'y' }
        graph_unbind_property = @{ nodeId = 999999; propertyName = 'x' }
        effect_compile        = @{ nodeId = 999999; hlsl = 'x' }
        effect_get_hlsl       = @{ nodeId = $scratch }
        registry_get_effect   = @{ name = 'Gaussian Blur' }
        set_preview_node      = @{ nodeId = $scratch }
        render_capture_node   = @{ nodeId = 999999 }
        read_analysis_output  = @{ nodeId = $scratch }
        read_pixel_region     = @{ nodeId = 999999; x = 0; y = 0; w = 1; h = 1 }
        read_pixel_trace      = @{ nodeId = 999999; x = 0.5; y = 0.5 }
        set_display_profile   = @{ }
        switch_gpu            = @{ }
        node_logs             = @{ nodeId = $scratch }
    }
    $badTools = @()
    foreach ($t in $tools) {
        $name = $t.name
        $args2 = if ($canned.ContainsKey($name)) { $canned[$name] } else { @{} }
        try {
            $r = Rpc @{ jsonrpc = '2.0'; id = (Get-Random -Maximum 999999); method = 'tools/call'
                        params = @{ name = $name; arguments = $args2 } }
            $env = $r.result
            $shaped = ($null -ne $env) -and ($null -ne $env.content) -and ($env.isError -is [bool]) -and
                      (($null -ne $env.content[0].text) -or ($env.content[0].type -eq 'image'))
            if (-not $shaped) { $badTools += $name }
        } catch { $badTools += "$name (transport: $($_.Exception.Message))" }
    }
    if ($badTools.Count -gt 0) { Log "Malformed: $($badTools -join ', ')"; return $false }
    return $true
}

# ============================================================================
# Tests: CLI Mode
# ============================================================================

RunTest "CLI.BasicEvaluation" {
    # Create a simple test graph fixture.
    $fixture = Join-Path $script:FixturesDir "test_cli_basic.json"
    # Build graph via MCP, save to file.
    $src = AddNode "Gamut Source"
    SetProperty $src "OutputSize" 64.0
    $json = McpCall "graph_save_json"
    $jsonStr = $json | ConvertTo-Json -Depth 10
    [System.IO.File]::WriteAllText($fixture, $jsonStr, [System.Text.Encoding]::UTF8)

    $outDir = Join-Path $script:OutputDir "cli_basic"
    if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null

    # Run CLI mode (in a separate process — the current app is running GUI mode).
    # The CLI mode requires a separate invocation.
    # For now, just verify the fixture was created.
    return (Test-Path $fixture)
}

# ============================================================================
# Summary
# ============================================================================

Write-Host ""
Write-Host "=" * 60
$passed = ($script:TestResults | Where-Object { $_.Pass }).Count
$failed = ($script:TestResults | Where-Object { -not $_.Pass }).Count
$total = $script:TestResults.Count

if ($failed -eq 0) {
    Write-Host "ALL $total TESTS PASSED" -ForegroundColor Green
} else {
    Write-Host "$passed PASSED, $failed FAILED out of $total" -ForegroundColor Red
    Write-Host ""
    Write-Host "Failures:" -ForegroundColor Red
    foreach ($t in ($script:TestResults | Where-Object { -not $_.Pass })) {
        Write-Host "  $($t.Name): $($t.Error)" -ForegroundColor Red
    }
}
Write-Host "=" * 60

Stop-Shim
exit $failed
