<#
.SYNOPSIS
    MCP broker smoke (stdio-migration Step 5): election, framing, idle
    exit, stdout hygiene, initialize / tools/list with no session attached.

.DESCRIPTION
    Runs entirely against a PRE-LAUNCHED unpackaged hub on an isolated
    pipe name (a fresh GUID per run — the readiness event derives from
    the pipe name, so one override isolates every named object). This is
    deliberate: no unpackaged activation path exists, so neither this
    script nor CI covers IApplicationActivationManager-based election of
    the PACKAGED hub — that stays a manual test (documented gap, not a
    pretended coverage).

    SHADERLAB_MCP_ALLOW_UNPACKAGED=1 is set for the duration: hub and
    shim are the same unpackaged binary in the same directory, which is
    exactly the dev/CI pairing fallback.
#>
param(
    [string]$Configuration = "Debug",
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "$Platform\$Configuration\ShaderLabMcpBroker\ShaderLabMcpBroker.exe"
$headless = Join-Path $root "$Platform\$Configuration\ShaderLabHeadless\ShaderLabHeadless.exe"
$fixture = Join-Path $PSScriptRoot "fixtures\test_cli_basic.json"
if (-not (Test-Path $exe)) {
    Write-Error "Broker not found at $exe -- build first."
    exit 1
}

$script:failures = 0
function Check($name, $cond) {
    if ($cond) { Write-Host "[PASS] $name" -ForegroundColor Green }
    else       { Write-Host "[FAIL] $name" -ForegroundColor Red; $script:failures++ }
}

$pipe = "ShaderLab.mcp.test.$([guid]::NewGuid().ToString('N'))"
$env:SHADERLAB_MCP_ALLOW_UNPACKAGED = '1'
Write-Host "Broker: $exe"
Write-Host "Pipe:   $pipe"

$hub = $null
$shim = $null
$session = $null
try {
    # ---- 1. Hub election winner comes up ----------------------------------
    $hub = Start-Process $exe -ArgumentList '--hub','--pipe',$pipe,'--idle-exit-sec','4' `
        -PassThru -WindowStyle Hidden
    $deadline = (Get-Date).AddSeconds(8)
    $up = $false
    while ((Get-Date) -lt $deadline) {
        if (Test-Path "\\.\pipe\$pipe") { $up = $true; break }
        if ($hub.HasExited) { break }
        Start-Sleep -Milliseconds 200
    }
    Check "Hub.PipeAppears" $up
    Check "Hub.StaysRunning" (-not $hub.HasExited)

    # ---- 2. Election: a second hub proves the incumbent and exits 0 -------
    $hub2 = Start-Process $exe -ArgumentList '--hub','--pipe',$pipe,'--idle-exit-sec','4' `
        -PassThru -WindowStyle Hidden
    $exited = $hub2.WaitForExit(8000)
    Check "Election.LoserExitsQuickly" $exited
    Check "Election.LoserExitCodeZero" ($exited -and $hub2.ExitCode -eq 0)
    Check "Election.WinnerSurvives" (-not $hub.HasExited)

    # ---- 3. Shim conversation over redirected stdio -----------------------
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $exe
    $psi.Arguments = "--stdio --pipe $pipe"
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $shim = [System.Diagnostics.Process]::Start($psi)

    function SendRaw([string]$line) { $shim.StandardInput.WriteLine($line) }
    function Send($obj) { SendRaw ($obj | ConvertTo-Json -Depth 8 -Compress) }
    function Recv($timeoutMs = 6000) {
        $task = $shim.StandardOutput.ReadLineAsync()
        if (-not $task.Wait($timeoutMs)) { return $null }
        return $task.Result
    }

    Send @{ jsonrpc = '2.0'; id = 1; method = 'initialize'; params = @{ protocolVersion = '2025-06-18' } }
    $line = Recv
    $init = if ($line) { $line | ConvertFrom-Json } else { $null }
    Check "Shim.InitializeAnswers" ($null -ne $init)
    Check "Shim.InitializeVersion" ($init.result.protocolVersion -eq '2025-06-18')
    Check "Shim.InitializeServerName" ($init.result.serverInfo.name -eq 'shaderlab-shim')
    Check "Shim.SingleLineJson" ($line -and -not $line.Contains("`r"))

    Send @{ jsonrpc = '2.0'; id = 2; method = 'tools/list' }
    $tl = (Recv) | ConvertFrom-Json
    $names = @($tl.result.tools | ForEach-Object name)
    Check "Shim.ToolsListSessionTools" (($names -contains 'list_sessions') -and ($names -contains 'use_session') -and $names.Count -eq 2)

    Send @{ jsonrpc = '2.0'; id = 3; method = 'tools/call'; params = @{ name = 'list_sessions'; arguments = @{} } }
    $ls = (Recv) | ConvertFrom-Json
    $sessionsOk = $false
    if ($ls.result.isError -eq $false) {
        try { $sessionsOk = @(($ls.result.content[0].text | ConvertFrom-Json).sessions).Count -eq 0 } catch {}
    } else {
        Write-Host "  list_sessions error text: $($ls.result.content[0].text)" -ForegroundColor DarkGray
    }
    Check "Shim.ListSessionsEmpty" $sessionsOk

    Send @{ jsonrpc = '2.0'; id = 4; method = 'tools/call'; params = @{ name = 'graph_add_node'; arguments = @{ effectName = 'x' } } }
    $ga = (Recv) | ConvertFrom-Json
    Check "Shim.GraphToolNoSessionError" (($ga.result.isError -eq $true) -and ($ga.result.content[0].text -match 'No session attached'))

    Send @{ jsonrpc = '2.0'; id = 5; method = 'tools/call'; params = @{ name = 'use_session'; arguments = @{ sessionId = 'no-such-session' } } }
    $usebad = (Recv) | ConvertFrom-Json
    Check "Shim.UseUnknownSessionError" (($usebad.result.isError -eq $true) -and ($usebad.result.content[0].text -match 'Unknown session'))

    # ---- 3b. End-to-end through a real headless session -------------------
    # This is the Step 6 gate: launch a headless session (WARP), register it
    # with the hub, list it, pin it, drive a real engine route through the
    # sealed shim->hub->session channel, and read the result back.
    if (Test-Path $headless) {
        $session = Start-Process $headless -ArgumentList `
            '--graph',$fixture,'--mcp-session','--pipe',$pipe, `
            '--session-label','smoke-session','--adapter','warp' -PassThru -WindowStyle Hidden

        # Wait for the session to register (list_sessions returns it).
        $sid = $null
        $deadline2 = (Get-Date).AddSeconds(30)
        while ((Get-Date) -lt $deadline2) {
            Send @{ jsonrpc = '2.0'; id = 100; method = 'tools/call'; params = @{ name = 'list_sessions'; arguments = @{} } }
            $r = (Recv) | ConvertFrom-Json
            if ($r.result.isError -eq $false) {
                $ss = @(($r.result.content[0].text | ConvertFrom-Json).sessions)
                if ($ss.Count -ge 1) { $sid = $ss[0].id; break }
            }
            if ($session.HasExited) { break }
            Start-Sleep -Milliseconds 500
        }
        Check "Session.Registers" ($null -ne $sid)

        if ($sid) {
            Send @{ jsonrpc = '2.0'; id = 101; method = 'tools/call'; params = @{ name = 'use_session'; arguments = @{ sessionId = $sid } } }
            $use = (Recv) | ConvertFrom-Json
            Check "Session.UseAttaches" ($use.result.isError -eq $false)

            # tools/list now splices the session's catalog in.
            Send @{ jsonrpc = '2.0'; id = 102; method = 'tools/list' }
            $spliced = @(((Recv) | ConvertFrom-Json).result.tools | ForEach-Object name)
            Check "Session.ToolsListSpliced" (($spliced -contains 'list_sessions') -and ($spliced -contains 'graph_add_node') -and ($spliced -contains 'graph_overview'))

            # Drive a real engine route end-to-end (sealed through the hub).
            Send @{ jsonrpc = '2.0'; id = 103; method = 'tools/call'; params = @{ name = 'graph_overview'; arguments = @{} } }
            $ov = (Recv) | ConvertFrom-Json
            $ovOk = $false
            if ($ov.result.isError -eq $false) {
                try { $ovOk = $null -ne ($ov.result.content[0].text | ConvertFrom-Json).nodes } catch {}
            }
            Check "Session.GraphOverviewRoundTrip" $ovOk

            Send @{ jsonrpc = '2.0'; id = 104; method = 'tools/call'; params = @{ name = 'graph_add_node'; arguments = @{ effectName = 'Gaussian Blur' } } }
            $add = (Recv) | ConvertFrom-Json
            $addOk = $false
            if ($add.result.isError -eq $false) {
                try { $addOk = ($add.result.content[0].text | ConvertFrom-Json).nodeId -ge 1 } catch {}
            }
            Check "Session.MutatingRouteRoundTrip" $addOk

            # session_gone: kill the session, then a forwarded call must
            # surface a distinct error, not a hang or silent success.
            Stop-Process -Id $session.Id -Force -ErrorAction SilentlyContinue
            $session.WaitForExit(5000) | Out-Null
            Start-Sleep -Milliseconds 800
            Send @{ jsonrpc = '2.0'; id = 105; method = 'tools/call'; params = @{ name = 'graph_overview'; arguments = @{} } }
            $gone = (Recv) | ConvertFrom-Json
            Check "Session.GoneSurfacesDistinctError" (($gone.result.isError -eq $true) -and ($gone.result.content[0].text -match 'session_gone|No session'))
        }
    } else {
        Write-Host "[SKIP] headless not built -- session end-to-end checks skipped" -ForegroundColor DarkYellow
    }

    # Notification must emit ZERO bytes: send one, then a ping — the very
    # next line back must be the ping reply.
    Send @{ jsonrpc = '2.0'; method = 'notifications/initialized' }
    Send @{ jsonrpc = '2.0'; id = 9; method = 'ping' }
    $ping = (Recv) | ConvertFrom-Json
    Check "Shim.NotificationSilent" ($ping.id -eq 9)

    SendRaw 'this is not json'
    $err = (Recv) | ConvertFrom-Json
    Check "Shim.ParseErrorShape" ($err.error.code -eq -32700)

    $shim.StandardInput.Close()
    $shimExited = $shim.WaitForExit(6000)
    Check "Shim.ExitsOnStdinClose" $shimExited
    Check "Shim.ExitCodeZero" ($shimExited -and $shim.ExitCode -eq 0)

    # stdout hygiene: everything received above parsed as JSON, and the
    # shim wrote nothing to stderr. (Checked after exit — StreamReader
    # peeks BLOCK on a live process with an empty stream.)
    $stderrText = if ($shimExited) { $shim.StandardError.ReadToEnd() } else { 'shim still running' }
    Check "Shim.StdErrQuiet" ([string]::IsNullOrEmpty($stderrText))

    # ---- 4. Idle exit ------------------------------------------------------
    # Last client gone; --idle-exit-sec 4 should take the hub down.
    $hubExited = $hub.WaitForExit(15000)
    Check "Hub.IdleExit" $hubExited
    Check "Hub.IdleExitCodeZero" ($hubExited -and $hub.ExitCode -eq 0)
}
finally {
    foreach ($p in @($shim, $session, $hub)) {
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
    Remove-Item Env:\SHADERLAB_MCP_ALLOW_UNPACKAGED -ErrorAction SilentlyContinue
}

Write-Host ""
if ($script:failures -eq 0) { Write-Host "BROKER SMOKE: ALL CHECKS PASSED" -ForegroundColor Green }
else { Write-Host "BROKER SMOKE: $($script:failures) FAILURE(S)" -ForegroundColor Red }
exit $script:failures
