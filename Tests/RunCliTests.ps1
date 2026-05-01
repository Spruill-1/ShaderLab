<#
.SYNOPSIS
    ShaderLab automated test suite. Runs built-in tests via CLI --test mode.

.DESCRIPTION
    Builds, deploys, and runs ShaderLab's built-in test suite in headless CLI mode.
    No GUI or MCP server needed — tests run entirely inside the app process.
    Reports pass/fail per test with exit code for CI.

.PARAMETER SkipBuild
    Skip MSBuild and deployment (use existing installation).

.PARAMETER Adapter
    GPU adapter: "default", "warp", or "both" (runs twice). Default: "both".
#>
param(
    [switch]$SkipBuild,
    [string]$Adapter = "both"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot -Parent
$MSBuild = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
$AppId = "ShaderLab_9v3yd384n9j18!App"

# ============================================================================
# Build & Deploy
# ============================================================================

if (-not $SkipBuild) {
    Write-Host "=== Building ShaderLab ===" -ForegroundColor Cyan
    Push-Location $RepoRoot
    & $MSBuild ShaderLab.vcxproj /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED" -ForegroundColor Red; exit 1 }
    Pop-Location

    Write-Host "=== Deploying ===" -ForegroundColor Cyan
    # Kill any running instances first.
    Get-Process ShaderLab -ErrorAction SilentlyContinue | ForEach-Object {
        try { Stop-Process -Id $_.Id -Force } catch {}
    }
    Start-Sleep -Seconds 2
    Add-AppxPackage -Register "$RepoRoot\x64\Debug\ShaderLab\AppxManifest.xml" 2>&1 | Out-Null
}

# ============================================================================
# Run Tests
# ============================================================================

function RunTestPass($adapterFlag) {
    $adapterLabel = if ($adapterFlag -eq "warp") { "WARP" } else { "Hardware" }
    Write-Host ""
    Write-Host "=== Running tests on $adapterLabel ===" -ForegroundColor Cyan

    # Launch the app in CLI test mode. It prints results to stdout and exits.
    $env:SHADERLAB_MCP = "0"
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "explorer.exe"
    $psi.Arguments = "shell:AppsFolder\$AppId"
    $psi.UseShellExecute = $false

    # The app is a packaged WinUI 3 app — we launch via explorer and it picks up
    # the command line from an environment variable since packaged apps can't easily
    # receive arbitrary CLI args via explorer.exe.
    # Instead, write a sentinel file that the app checks on startup.
    $sentinelDir = "$env:LOCALAPPDATA\ShaderLab"
    New-Item -ItemType Directory -Path $sentinelDir -Force | Out-Null
    $sentinelFile = Join-Path $sentinelDir "test_args.txt"
    Set-Content -Path $sentinelFile -Value "--cli --test --adapter $adapterFlag"

    # Actually, packaged apps get their command line from the activation.
    # Let's use the exe directly instead.
    $exePath = "$RepoRoot\x64\Debug\ShaderLab\ShaderLab.exe"
    if (-not (Test-Path $exePath)) {
        Write-Host "ERROR: $exePath not found" -ForegroundColor Red
        return 1
    }

    # Run the test directly. The exe handles console attach.
    $output = & $exePath --cli --test --adapter $adapterFlag 2>&1
    $exitCode = $LASTEXITCODE

    # Print output.
    foreach ($line in $output) {
        if ($line -match '\[PASS\]') {
            Write-Host $line -ForegroundColor Green
        } elseif ($line -match '\[FAIL\]') {
            Write-Host $line -ForegroundColor Red
        } elseif ($line -match 'ALL.*PASSED') {
            Write-Host $line -ForegroundColor Green
        } elseif ($line -match 'FAILED') {
            Write-Host $line -ForegroundColor Red
        } else {
            Write-Host $line
        }
    }

    return $exitCode
}

$totalFailures = 0

if ($Adapter -eq "both" -or $Adapter -eq "default") {
    $totalFailures += RunTestPass "default"
}

if ($Adapter -eq "both" -or $Adapter -eq "warp") {
    $totalFailures += RunTestPass "warp"
}

Write-Host ""
if ($totalFailures -eq 0) {
    Write-Host "ALL TEST PASSES SUCCEEDED" -ForegroundColor Green
} else {
    Write-Host "TOTAL FAILURES: $totalFailures" -ForegroundColor Red
}

exit $totalFailures
