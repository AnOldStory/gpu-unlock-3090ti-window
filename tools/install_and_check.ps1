<#
.SYNOPSIS
  Install the payload on the internal EFI System Partition, then verify it.

.DESCRIPTION
  Convenience wrapper: runs install_esp_entry.ps1 and then check_status.ps1 in
  the same elevated session, so only one UAC prompt is needed. The verification
  output is exported to the check script's -Export directory.

.EXAMPLE
  # elevated
  powershell -ExecutionPolicy Bypass -File tools\install_and_check.ps1
#>
[CmdletBinding()]
param(
  [ValidateSet('profile', 'unlock')] [string]$Variant = 'profile',
  [switch]$NoOneTime
)

$install = Join-Path $PSScriptRoot 'install_esp_entry.ps1'
$check   = Join-Path $PSScriptRoot 'check_status.ps1'
if (-not (Test-Path $install) -or -not (Test-Path $check)) { throw 'Missing sibling scripts.' }

$arguments = @{ Action = 'Install'; Variant = $Variant; Esp = $true }
if (-not $NoOneTime) { $arguments['OneTime'] = $true }

& $install @arguments
Write-Host "`n================ verification ================`n"
& $check -Esp

# Leave the machine with no extra drive letter mapped.
& mountvol S: /D 2>$null | Out-Null
Write-Host "`ndone."
