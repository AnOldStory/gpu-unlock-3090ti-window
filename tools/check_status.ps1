<#
.SYNOPSIS
  Read-only status check for the RTX 3090 Ti UEFI payload installed on an
  internal FAT32 volume.

.DESCRIPTION
  Prints and exports: firmware/Secure Boot state, the payload files on the
  target volume, the probe report (with the profile values) if one exists, the
  PROM hash, and the BCD entry. It changes nothing except mounting the target
  volume while it runs, and it exports everything to -Export so the result can
  be inspected afterwards.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\check_status.ps1 -Esp
#>
[CmdletBinding()]
param(
  [string]$Drive = 'S:',
  [switch]$Esp,
  [string]$Run = 'RUN00',
  [string]$Export = (Join-Path $env:TEMP 'gpu-unlock-export')
)

$ErrorActionPreference = 'Continue'
$EntryDesc  = 'GPU Unlock (UEFI)'
$ProbeDir   = 'RTX3090TI-PROBE'
$PayloadDir = 'RTX3090TI-UNLOCK'
$Drive      = $Drive.TrimEnd('\')
$Letter     = $Drive.TrimEnd(':')
New-Item -ItemType Directory -Force -Path $Export | Out-Null

Start-Transcript -Path (Join-Path $Export 'status.txt') -Force | Out-Null
try {
  $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

  Write-Host "== status =="
  Write-Host "elevated   : $admin"
  Write-Host "firmware   : $((Get-ComputerInfo -Property BiosFirmwareType).BiosFirmwareType)"
  Write-Host "last boot  : $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime)"
  Write-Host "now        : $(Get-Date)"

  $key = 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State'
  $sb = $null
  if (Test-Path $key) { $sb = (Get-ItemProperty -Path $key -Name UEFISecureBootEnabled).UEFISecureBootEnabled }
  if ($null -eq $sb) { Write-Host "secure boot: unknown" }
  elseif ($sb -eq 1)  { Write-Host "secure boot: ENABLED - the payload cannot load" }
  else                { Write-Host "secure boot: DISABLED - ok" }

  Get-CimInstance Win32_Volume | Where-Object { $_.FileSystem -eq 'FAT32' } | ForEach-Object {
    Write-Host ("fat32 vol  : {0} {1} free={2:N0} size={3:N0}" -f $_.DriveLetter, $_.DeviceID, $_.FreeSpace, $_.Capacity)
  }

  $mounted = $false
  if ($admin) {
    if (-not (Test-Path "$Drive\")) {
      if ($Esp) { & mountvol $Drive /S | Out-Host; $mounted = Test-Path "$Drive\" }
      else { Write-Host "$Drive is not present; pass -Esp or the right -Drive." }
    }
    if (Test-Path "$Drive\") {
      Write-Host "`n-- payload files on $Drive --"
      foreach ($p in @("EFI\gpulock\BOOTX64.EFI", "EFI\gpulock", $ProbeDir, $PayloadDir, "$PayloadDir\ASSETS")) {
        $item = Get-Item -LiteralPath "$Drive\$p" -Force -ErrorAction SilentlyContinue
        if ($item) {
          if ($item.PSIsContainer) { Write-Host ("dir   {0}" -f $p) }
          else {
            Write-Host ("file  {0,-34} {1,8} bytes  {2}  {3}" -f $p, $item.Length, $item.LastWriteTime,
              (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.Substring(0, 16))
          }
        } else { Write-Host ("MISSING {0}" -f $p) }
      }

      foreach ($run in (Get-ChildItem -LiteralPath "$Drive\$ProbeDir" -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)) {
        foreach ($name in @('REPORT.TXT', 'PROM.BIN')) {
          $src = "$Drive\$ProbeDir\$run\$name"
          if (Test-Path -LiteralPath $src) {
            $dst = Join-Path $Export ("{0}-{1}" -f $run, $name)
            Copy-Item -LiteralPath $src -Destination $dst -Force
          }
        }
      }

      $report = "$Drive\$ProbeDir\$Run\REPORT.TXT"
      if (Test-Path -LiteralPath $report) {
        Write-Host "`n-- $ProbeDir\$Run\REPORT.TXT --"
        Get-Content -LiteralPath $report
      } else {
        Write-Host "`nno probe report at $ProbeDir\$Run\REPORT.TXT"
        $any = Get-ChildItem -LiteralPath "$Drive\$ProbeDir" -Recurse -ErrorAction SilentlyContinue
        if ($any) { Write-Host "contents of ${ProbeDir}:"; $any | Select-Object FullName, Length, LastWriteTime | Format-Table -AutoSize }
      }

      $prom = "$Drive\$ProbeDir\$Run\PROM.BIN"
      if (Test-Path -LiteralPath $prom) {
        Write-Host ("`nPROM.BIN sha256: " + (Get-FileHash -LiteralPath $prom -Algorithm SHA256).Hash.ToLower())
      }
    }

    Write-Host "`n-- BCD entries --"
    $out = & bcdedit /enum all 2>&1
    $block = @()
    $found = $false
    foreach ($line in $out) {
      if ($line -match '^\s*identifier\s') {
        if ($block.Count -and (($block -join "`n") -match [regex]::Escape($EntryDesc))) { $block | ForEach-Object { Write-Host $_ }; Write-Host ""; $found = $true }
        $block = @()
      }
      $block += $line
    }
    if ($block.Count -and (($block -join "`n") -match [regex]::Escape($EntryDesc))) { $block | ForEach-Object { Write-Host $_ }; $found = $true }
    if (-not $found) { Write-Host "no '$EntryDesc' entry: the payload is not installed yet" }

    Write-Host "`n-- firmware boot order --"
    & bcdedit /enum "{fwbootmgr}" 2>&1 | Select-Object -First 24
  } else {
    Write-Host "`nNot elevated: the EFI System Partition and the BCD store cannot be read."
    Write-Host "Re-run in an elevated PowerShell (Run as administrator) with -Esp."
  }
} finally {
  if ($mounted) { & mountvol $Drive /D | Out-Host }
  Stop-Transcript | Out-Null
}
