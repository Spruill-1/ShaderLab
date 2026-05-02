# Generates a local self-signed code-signing certificate used to sign the MSIX
# package for local F5 deploy. The .pfx is gitignored and never checked in.
# This script is invoked automatically by the EnsureDevSigningCertificate target
# in ShaderLab.vcxproj on a fresh clone. It can also be run manually.
#
# The Subject CN must match the Publisher in Package.appxmanifest (CN=ShaderLab).
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)] [string] $PfxPath,
	[Parameter(Mandatory = $true)] [string] $Password,
	[string] $Subject = 'CN=ShaderLab',
	[string] $FriendlyName = 'ShaderLab Dev Cert'
)

$ErrorActionPreference = 'Stop'

# Ensure the Certificate PSDrive provider is available. When PowerShell is launched
# with -File from a non-interactive parent (e.g. MSBuild), the security module that
# registers the Cert: drive is sometimes not auto-loaded. Module load can emit benign
# type-data warnings, so swallow them entirely.
try { Import-Module Microsoft.PowerShell.Security -ErrorAction SilentlyContinue -WarningAction SilentlyContinue *>$null } catch { }
try { Import-Module PKI -ErrorAction SilentlyContinue -WarningAction SilentlyContinue *>$null } catch { }
if (-not (Get-PSDrive -Name Cert -ErrorAction SilentlyContinue)) {
    try { $null = New-PSDrive -Name Cert -PSProvider Certificate -Root '\' -ErrorAction SilentlyContinue } catch { }
}

if (Test-Path -LiteralPath $PfxPath) {
	Write-Host "Dev cert already exists at $PfxPath. Skipping."
	exit 0
}

Write-Host "Creating self-signed code-signing certificate: $Subject"
$cert = New-SelfSignedCertificate `
	-Type CodeSigningCert `
	-Subject $Subject `
	-KeyUsage DigitalSignature `
	-FriendlyName $FriendlyName `
	-CertStoreLocation 'Cert:\CurrentUser\My' `
	-TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}') `
	-NotAfter (Get-Date).AddYears(5)

$securePwd = ConvertTo-SecureString -String $Password -Force -AsPlainText
$null = Export-PfxCertificate -Cert ("Cert:\CurrentUser\My\" + $cert.Thumbprint) -FilePath $PfxPath -Password $securePwd
Write-Host "Exported PFX to $PfxPath"

# Importing into LocalMachine\TrustedPeople requires admin. If MSBuild isn't elevated,
# fall back to CurrentUser\TrustedPeople, which is sufficient for per-user MSIX deploy.
try {
	$null = Import-PfxCertificate -FilePath $PfxPath -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' -Password $securePwd
	Write-Host "Cert trusted in LocalMachine\TrustedPeople."
}
catch {
	Write-Warning "Could not write LocalMachine\TrustedPeople (not elevated). Falling back to CurrentUser\TrustedPeople."
	$null = Import-PfxCertificate -FilePath $PfxPath -CertStoreLocation 'Cert:\CurrentUser\TrustedPeople' -Password $securePwd
	Write-Host "Cert trusted in CurrentUser\TrustedPeople."
}
