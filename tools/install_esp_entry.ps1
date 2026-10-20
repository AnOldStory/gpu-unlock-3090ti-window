<#
.SYNOPSIS
  Run the RTX 3090 Ti UEFI payload from an internal FAT32 volume (no USB stick),
  by registering a Windows Boot Manager entry that launches it.

.DESCRIPTION
  Windows cannot run the unlock itself: the NVIDIA driver owns the GPU, and the
  sequence needs an FLR before that driver binds. A reboot into a pre-Windows
  context is therefore unavoidable. What this script removes is the USB
  requirement.

  Two internal destinations work:

    ESP      the machine's own EFI System Partition (100 MB on this PC).
             Perfect for the 31 KB profiler. Too small for the unlock payload:
             its five firmware assets total ~85 MB.
    FAT32    any other FAT32 partition, e.g. a 200 MB partition carved out of a
             data disk. Holds either payload.

  Nothing here touches the GPU. The default action is DryRun: it prints the
  exact commands and changes nothing.

.PARAMETER Action
  DryRun (default) prints the plan. Install copies files and adds the BCD entry.
  Remove deletes the BCD entry and the copied files.

.PARAMETER Variant
  profile = build/TI_PROFILE.EFI (read-only profiler, no firmware assets).
  unlock  = unlock/build/BOOTX64.EFI (requires -Assets).

.PARAMETER Drive
  Target volume, e.g. S: or G:.

.PARAMETER Esp
  Treat -Drive as the EFI System Partition and mount it with 'mountvol /S'.

.PARAMETER Assets
  Directory holding the five generator outputs. Verified against
  unlock/assets.json (size + SHA-256) before anything is copied.

.PARAMETER OneTime
  Boot the payload once (bcdedit /bootsequence) instead of adding it to the
  permanent boot menu.

.EXAMPLE
  # review only
  powershell -ExecutionPolicy Bypass -File tools\install_esp_entry.ps1 -Action DryRun -Variant profile -Esp -OneTime

.EXAMPLE
  # elevated: profiler from the ESP, one-time boot
  powershell -ExecutionPolicy Bypass -File tools\install_esp_entry.ps1 -Action Install -Variant profile -Esp -OneTime

.EXAMPLE
  # elevated: unlocker from a dedicated FAT32 partition
  powershell -ExecutionPolicy Bypass -File tools\install_esp_entry.ps1 -Action Install -Variant unlock -Drive G: -Assets .\assets -OneTime

.EXAMPLE
  # elevated: undo
  powershell -ExecutionPolicy Bypass -File tools\install_esp_entry.ps1 -Action Remove -Esp
#>
[CmdletBinding()]
param(
  [ValidateSet('DryRun', 'Install', 'Remove')] [string]$Action = 'DryRun',
  [ValidateSet('profile', 'unlock')] [string]$Variant = 'profile',
  [string]$Drive = 'S:',
  [switch]$Esp,
  [string]$Assets,
  [switch]$OneTime
)

$ErrorActionPreference = 'Stop'
$Root       = Split-Path -Parent $PSScriptRoot
$EntryDesc  = 'GPU Unlock (UEFI)'
$EspFolder  = 'EFI\gpulock'
$EspApp     = 'EFI\gpulock\BOOTX64.EFI'
$ProbeDir   = 'RTX3090TI-PROBE'
$PayloadDir = 'RTX3090TI-UNLOCK'
$Source     = if ($Variant -eq 'profile') { Join-Path $Root 'build\TI_PROFILE.EFI' }
              else { Join-Path $Root 'unlock\build\BOOTX64.EFI' }
$Drive      = $Drive.TrimEnd('\')
$Letter     = $Drive.TrimEnd(':')

function Test-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  return ([Security.Principal.WindowsPrincipal]$id).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-SecureBootState {
  # The registry mirrors the UEFI variable and needs no elevation.
  $key = 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State'
  if (Test-Path $key) {
    $value = Get-ItemProperty -Path $key -Name UEFISecureBootEnabled -ErrorAction SilentlyContinue
    if ($null -ne $value) { return [bool]$value.UEFISecureBootEnabled }
  }
  try { return (Confirm-SecureBootUEFI) } catch { return $null }
}

function Get-EspEntryGuid {
  $out = & bcdedit /enum all 2>$null
  $guid = $null
  foreach ($line in $out) {
    if ($line -match '^\s*identifier\s+\{([0-9a-fA-F\-]{36})\}') { $guid = $Matches[1] }
    if ($line -match ("^\s*description\s+" + [regex]::Escape($EntryDesc) + "\s*$") -and $guid) { return $guid }
  }
  return $null
}

function Test-EfiApp {
  param([string]$Path)
  $bytes = [IO.File]::ReadAllBytes($Path)
  if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return $false }
  $pe = [BitConverter]::ToUInt32($bytes, 0x3C)
  if ($pe + 24 -gt $bytes.Length) { return $false }
  if ($bytes[$pe] -ne 0x50 -or $bytes[$pe + 1] -ne 0x45) { return $false }
  $magic = [BitConverter]::ToUInt16($bytes, $pe + 24)
  $subsystem = [BitConverter]::ToUInt16($bytes, $pe + 24 + 68)
  return ($magic -eq 0x20B -and $subsystem -eq 10)
}

function Get-AssetPlan {
  $specPath = Join-Path $Root 'unlock\assets.json'
  if (-not (Test-Path $specPath)) { throw "Missing $specPath" }
  $specs = Get-Content -Raw -LiteralPath $specPath | ConvertFrom-Json
  if (-not $Assets) { throw "-Variant unlock needs -Assets <directory>." }
  $directory = (Resolve-Path -LiteralPath $Assets).Path
  $total = 0
  $plan = foreach ($spec in $specs) {
    $file = Join-Path $directory $spec.Name
    if (-not (Test-Path -LiteralPath $file)) { throw "Missing asset $($spec.Name) in $directory" }
    $item = Get-Item -LiteralPath $file
    if ($item.Length -ne $spec.Bytes) { throw "$($spec.Name): size $($item.Length) != $($spec.Bytes)" }
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $spec.Sha256.ToLower()) { throw "$($spec.Name): sha256 mismatch" }
    $total += $item.Length
    [pscustomobject]@{ Name = $spec.Name; Path = $file; Bytes = $item.Length }
  }
  return @{ Files = $plan; Bytes = $total }
}

Write-Host "== GPU unlock boot entry ==" -ForegroundColor Cyan
Write-Host "action     : $Action"
Write-Host "variant    : $Variant"
Write-Host "source     : $Source"
Write-Host ("exists     : {0}" -f (Test-Path -LiteralPath $Source))
Write-Host "destination: $Drive ($(if ($Esp) { 'EFI System Partition' } else { 'FAT32 partition' }))"
Write-Host "esp app    : $EspApp"
Write-Host "one-time   : $([bool]$OneTime)"

if (-not (Test-Path -LiteralPath $Source)) {
  throw "Missing $Source. Build it first: bash unlock/build-wsl.sh $Variant"
}
if (-not (Test-EfiApp $Source)) { throw "$Source is not a PE32+ EFI application." }
$size = (Get-Item -LiteralPath $Source).Length
Write-Host ("efi        : {0} bytes, PE32+ EFI application" -f $size)

$assetPlan = $null
if ($Variant -eq 'unlock') {
  $assetPlan = Get-AssetPlan
  Write-Host ("assets     : {0} files, {1:N0} bytes verified against unlock/assets.json" -f
    $assetPlan.Files.Count, $assetPlan.Bytes)
}

$sb = Get-SecureBootState
if ($null -eq $sb)      { Write-Host "secure boot: unknown - check the firmware settings yourself" -ForegroundColor Yellow }
elseif ($sb)            { Write-Host "secure boot: ENABLED - an unsigned EFI app will not load; disable it first" -ForegroundColor Red }
else                    { Write-Host "secure boot: disabled - ok" -ForegroundColor Green }
Write-Host "bitlocker  : have the recovery key ready before you change firmware settings" -ForegroundColor Yellow

if (-not (Test-Admin)) {
  if ($Action -eq 'DryRun') {
    Write-Host "not elevated: DryRun still prints the plan, but cannot inspect the volume or the BCD." -ForegroundColor Yellow
  } else {
    throw "Run this in an elevated PowerShell (Run as administrator)."
  }
}

$mounted = $false
$guid = $null
$free = $null

if (Test-Admin) {
  Write-Host "`n-- volume --"
  $present = Test-Path "$Drive\"
  if ($Esp -and -not $present) {
    Write-Host "mountvol $Drive /S"
    if ($Action -ne 'DryRun') { & mountvol $Drive /S | Out-Host; $mounted = $true; $present = Test-Path "$Drive\" }
  }
  if ($Esp -and $present -and $Action -ne 'DryRun' -and -not (Test-Path "$Drive\EFI")) {
    throw "$Drive has no \EFI folder; it does not look like an ESP. Pass -Drive with a free letter."
  }
  if (-not $Esp -and $Action -ne 'DryRun') {
    if (-not $present) { throw "$Drive is not present. Create and format the partition first." }
    $volume = Get-Volume -DriveLetter $Letter -ErrorAction SilentlyContinue
    if (-not $volume) { throw "No volume on $Drive." }
    if ($volume.FileSystem -ne 'FAT32') { throw "$Drive is $($volume.FileSystem); the firmware only reads FAT32 here." }
  }
  if ($present -or $mounted) {
    $volume = Get-Volume -DriveLetter $Letter -ErrorAction SilentlyContinue
    if ($volume) {
      $free = $volume.SizeRemaining
      Write-Host ("free space : {0:N0} bytes" -f $free)
    }
  }
  $guid = Get-EspEntryGuid
  if ($guid) { Write-Host "existing bcd entry: {$guid}" }
}

$needed = $size + 8MB + $(if ($assetPlan) { $assetPlan.Bytes } else { 0 })
if ($null -ne $free -and $free -lt $needed) {
  throw ("Not enough free space: need ~{0:N0} bytes, {1:N0} available." -f $needed, $free)
}

switch ($Action) {
  'DryRun' {
    Write-Host "`n-- plan --"
    $step = 0
    if ($Esp) { Write-Host ("{0}. mountvol {1} /S" -f (++$step), $Drive) }
    Write-Host ("{0}. create {1}\{2}" -f (++$step), $Drive, $EspFolder)
    Write-Host ("{0}. copy '{1}' -> {2}\{3}" -f (++$step), $Source, $Drive, $EspApp)
    Write-Host ("{0}. create {1}\{2} and {1}\{3}\ASSETS" -f (++$step), $Drive, $ProbeDir, $PayloadDir)
    if ($assetPlan) {
      Write-Host ("{0}. copy and verify {1} asset(s) ({2:N0} bytes) into {3}\{4}\ASSETS" -f
        (++$step), $assetPlan.Files.Count, $assetPlan.Bytes, $Drive, $PayloadDir)
    }
    Write-Host ("{0}. bcdedit /copy {{bootmgr}} /d `"{1}`"" -f (++$step), $EntryDesc)
    Write-Host ("{0}. bcdedit /set {{new}} device partition={1}" -f (++$step), $Drive)
    Write-Host ("{0}. bcdedit /set {{new}} path \{1}" -f (++$step), $EspApp)
    if ($OneTime) { Write-Host ("{0}. bcdedit /set {{fwbootmgr}} bootsequence {{new}}   (one boot only)" -f (++$step)) }
    else          { Write-Host ("{0}. bcdedit /set {{fwbootmgr}} displayorder {{new}} /addfirst" -f (++$step)) }
    Write-Host "`nReboot. The entry runs once, the app returns to the firmware, and Windows boots."
    Write-Host "If the app reports STOP, power the machine off instead of continuing."
  }
  'Install' {
    Write-Host "`n-- install --"
    New-Item -ItemType Directory -Force -Path "$Drive\$EspFolder"             | Out-Null
    New-Item -ItemType Directory -Force -Path "$Drive\$ProbeDir"              | Out-Null
    New-Item -ItemType Directory -Force -Path "$Drive\$PayloadDir\ASSETS"     | Out-Null
    Copy-Item -LiteralPath $Source -Destination "$Drive\$EspApp" -Force
    $copied = (Get-FileHash -LiteralPath "$Drive\$EspApp" -Algorithm SHA256).Hash
    $original = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    if ($copied -ne $original) { throw "Copy verification failed." }
    Write-Host "copied EFI, sha256=$copied"

    if ($assetPlan) {
      foreach ($entry in $assetPlan.Files) {
        $target = Join-Path "$Drive\$PayloadDir\ASSETS" $entry.Name
        Copy-Item -LiteralPath $entry.Path -Destination $target -Force
        $hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLower()
        $expect = (Get-Content -Raw -LiteralPath (Join-Path $Root 'unlock\assets.json') | ConvertFrom-Json |
                   Where-Object Name -eq $entry.Name).Sha256.ToLower()
        if ($hash -ne $expect) { throw "$($entry.Name): copied file does not match assets.json" }
        Write-Host "asset $($entry.Name) ok"
      }
      # Final pass. A copy that dies part way through must not leave a volume
      # that looks installed: the app verifies the SHA-256 of every asset, so a
      # missing file only surfaces as a refuse-to-start at boot.
      $missing = @()
      foreach ($entry in $assetPlan.Files) {
        $target = Join-Path "$Drive\$PayloadDir\ASSETS" $entry.Name
        if (-not (Test-Path -LiteralPath $target)) { $missing += "$($entry.Name) (missing)"; continue }
        $length = (Get-Item -LiteralPath $target).Length
        if ($length -ne $entry.Bytes) { $missing += "$($entry.Name) ($length != $($entry.Bytes) bytes)" }
      }
      if ($missing.Count) { throw "incomplete asset set on ${Drive}: $($missing -join '; ')" }
      Write-Host ("assets verified on target: {0} files, {1:N0} bytes" -f $assetPlan.Files.Count, $assetPlan.Bytes)
    }

    if ($guid) {
      Write-Host "reusing existing entry {$guid}"
    } else {
      $out = & bcdedit /copy "{bootmgr}" /d "$EntryDesc" 2>&1
      Write-Host $out
      if ($out -match '\{([0-9a-fA-F\-]{36})\}') { $guid = $Matches[1] }
      else { throw "Could not parse the new BCD entry from bcdedit output." }
    }
    & bcdedit /set "{${guid}}" device "partition=$Drive" | Out-Host
    & bcdedit /set "{${guid}}" path "\$EspApp"           | Out-Host
    # The firmware, not Windows Boot Manager, loads this file, so the one-time
    # boot has to be set on the firmware boot manager ({fwbootmgr} bootsequence
    # = the UEFI BootNext variable). 'bcdedit /bootsequence' would only affect
    # Windows Boot Manager, which is not in this chain.
    if ($OneTime) { & bcdedit /set "{fwbootmgr}" bootsequence "{${guid}}" | Out-Host }
    else          { & bcdedit /set "{fwbootmgr}" displayorder "{${guid}}" /addfirst | Out-Host }

    Write-Host "`ninstalled: {$guid}" -ForegroundColor Green
    Write-Host "Reboot, then the payload runs once and Windows boots."
    Write-Host "Undo: powershell -ExecutionPolicy Bypass -File tools\install_esp_entry.ps1 -Action Remove $(if ($Esp) { '-Esp' })"
  }
  'Remove' {
    Write-Host "`n-- remove --"
    if ($guid) {
      & bcdedit /set "{fwbootmgr}" displayorder "{${guid}}" /remove 2>$null | Out-Host
      & bcdedit /delete "{${guid}}" | Out-Host
      Write-Host "deleted bcd entry {$guid} and its firmware boot entry"
    } else {
      Write-Host "no BCD entry named '$EntryDesc'"
    }
    if (Test-Path "$Drive\$EspApp")    { Remove-Item -Force "$Drive\$EspApp";      Write-Host "removed $EspApp" }
    if (Test-Path "$Drive\$EspFolder") { Remove-Item -Force -Recurse "$Drive\$EspFolder" }
    Write-Host "kept $ProbeDir and $PayloadDir so REPORT.TXT, PROM.BIN and the assets stay readable" -ForegroundColor Yellow
  }
}

if ($mounted) { Write-Host "`n$Drive is still mapped. Release it with: mountvol $Drive /D" }
