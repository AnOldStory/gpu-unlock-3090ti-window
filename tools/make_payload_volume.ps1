<#
.SYNOPSIS
  Create (or remove) the dedicated FAT32 volume that holds the RTX 3090 Ti
  unlock payload, without a USB stick.

.DESCRIPTION
  The unlock app reads its payload (about 85 MB of firmware assets) and writes
  its report from the volume it was loaded from, and the firmware can only read
  FAT32. The machine's own ESP is 100 MB with ~64 MB free, so it cannot hold the
  assets. This script carves a small FAT32 partition out of a data disk instead.

  The new partition gets the EFI System Partition type GUID (EF00). That is what
  makes UEFI firmware enumerate it as a filesystem; a plain "basic data" FAT32
  partition is frequently not exposed to UEFI at all. Two ESPs on one machine
  are allowed by the specification.

  Nothing here touches the GPU or the boot configuration.

  Actions:
    Inspect  read-only: disks, partitions, BitLocker, shrinkable space, ESP,
             existing BCD entry. Run this first.
    Create   shrink -ShrinkDrive by -SizeMB + a slice of slack, create and
             format the new partition, and mount it at -DriveLetter.
    Remove   delete the payload partition and grow -ShrinkDrive back.

.PARAMETER Action
  Inspect (default), Create or Remove.

.PARAMETER DiskNumber
  Disk that holds -ShrinkDrive. Required for Create and Remove; Inspect prints it.

.PARAMETER ShrinkDrive
  Existing volume to take the space from. Default D:.

.PARAMETER SizeMB
  Size of the new partition, in MiB. Default 256 (the assets need ~81 MiB).
  260 MiB is reserved from -ShrinkDrive so the new partition always fits.

.PARAMETER DriveLetter
  Letter to mount the new volume at. Default: first free letter from G: up.

.PARAMETER Export
  Also write the console output to this file (used for elevated runs).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\make_payload_volume.ps1 -Action Inspect

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\make_payload_volume.ps1 -Action Create -DiskNumber 1 -ShrinkDrive D
#>
[CmdletBinding()]
param(
  [ValidateSet('Inspect', 'Create', 'Remove')] [string]$Action = 'Inspect',
  [int]$DiskNumber = -1,
  [string]$ShrinkDrive = 'D',
  [int]$SizeMB = 256,
  [string]$DriveLetter,
  [string]$Export
)

$ErrorActionPreference = 'Stop'
$ReserveMB = 4                                  # extra MiB taken from -ShrinkDrive
$Label     = 'GPULOCK'
$EspType   = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'   # EFI System Partition

function Write-Line {
  param([string]$Text, [string]$Color = 'Gray')
  Write-Host $Text -ForegroundColor $Color
  if ($Export) { Add-Content -LiteralPath $Export -Value $Text -Encoding UTF8 }
}

function Test-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  return ([Security.Principal.WindowsPrincipal]$id).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-BitLockerState {
  try {
    $volumes = Get-BitLockerVolume -ErrorAction Stop
    return $volumes | Select-Object MountPoint, VolumeStatus, ProtectionStatus, EncryptionPercentage
  } catch {
    return $null
  }
}

if ($Export) { Remove-Item -LiteralPath $Export -Force -ErrorAction SilentlyContinue }
Write-Line "== GPU unlock payload volume =="
Write-Line "action      : $Action"
Write-Line "shrink drive: ${ShrinkDrive}: by $($SizeMB + $ReserveMB) MiB"
Write-Line "new volume  : $SizeMB MiB, FAT32, label $Label, type EF00 (ESP)"
Write-Line "elevated    : $(Test-Admin)"

$sbKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State'
if (Test-Path $sbKey) {
  $sb = (Get-ItemProperty -Path $sbKey -Name UEFISecureBootEnabled -ErrorAction SilentlyContinue).UEFISecureBootEnabled
  if ($null -ne $sb) { Write-Line "secure boot : $([bool]$sb)$(if ($sb) { ' (must be off for the payload to load)' })" }
}

if (-not (Test-Admin)) {
  if ($Action -eq 'Inspect') {
    Write-Line "not elevated: disk, BitLocker and BCD details will be incomplete." 'Yellow'
  } else {
    throw 'Run this in an elevated PowerShell (Run as administrator).'
  }
}

Write-Line ''
Write-Line '-- disks --' 'Cyan'
if (Test-Admin) {
  Get-Disk | Sort-Object Number | ForEach-Object {
    Write-Line ("disk {0}: {1:N1} GB, {2}, {3}, {4}, boot={5}" -f
      $_.Number, ($_.Size / 1GB), $_.PartitionStyle, $_.BusType, $_.OperationalStatus, $_.IsBoot)
  }

  Write-Line ''
  Write-Line '-- partitions --' 'Cyan'
  Get-Partition | Sort-Object DiskNumber, PartitionNumber | ForEach-Object {
    $letter = if ($_.DriveLetter) { "$($_.DriveLetter):" } else { '' }
    $size = if ($_.Size -ge 1GB) { "{0:N1} GB" -f ($_.Size / 1GB) } else { "{0:N0} MB" -f ($_.Size / 1MB) }
    Write-Line ("disk {0} part {1}: {2,-4} {3,10} type={4} offset={5}" -f
      $_.DiskNumber, $_.PartitionNumber, $letter, $size, $_.GptType, $_.Offset)
  }

  Write-Line ''
  Write-Line '-- volumes --' 'Cyan'
  Get-Volume | Where-Object DriveLetter | Sort-Object DriveLetter | ForEach-Object {
    Write-Line ("{0}: {1,-6} size={2:N0} free={3:N0} label={4}" -f
      $_.DriveLetter, $_.FileSystem, $_.Size, $_.SizeRemaining, $_.FileSystemLabel)
  }

  Write-Line ''
  Write-Line '-- bitlocker --' 'Cyan'
  $bitlocker = Get-BitLockerState
  if ($bitlocker) {
    $bitlocker | ForEach-Object { Write-Line ("{0}: status={1} protection={2} encrypted={3}%" -f
      $_.MountPoint, $_.VolumeStatus, $_.ProtectionStatus, $_.EncryptionPercentage) }
  } else { Write-Line 'BitLocker information unavailable (module missing or not permitted).' }

  Write-Line ''
  Write-Line "-- shrinkable space on ${ShrinkDrive}: --" 'Cyan'
  try {
    $supported = Get-PartitionSupportedSize -DriveLetter $ShrinkDrive
    $current = (Get-Partition -DriveLetter $ShrinkDrive).Size
    Write-Line ("current={0:N0} min={1:N0} shrinkable={2:N0} bytes, target={3:N0} bytes" -f
      $current, $supported.SizeMin, ($current - $supported.SizeMin), ($current - ($SizeMB + $ReserveMB) * 1MB))
    if ($current - ($SizeMB + $ReserveMB) * 1MB -lt $supported.SizeMin) {
      Write-Line 'NOT ENOUGH SHRINKABLE SPACE' 'Red'
    } else {
      Write-Line 'ok: the requested shrink fits.' 'Green'
    }
  } catch {
    Write-Line "cannot query ${ShrinkDrive}: $($_.Exception.Message)" 'Red'
  }

  Write-Line ''
  Write-Line '-- payload partition already present? --' 'Cyan'
  $existing = Get-Partition | Where-Object { $_.GptType -eq $EspType } | ForEach-Object {
    $v = Get-Volume -Partition $_ -ErrorAction SilentlyContinue
    if ($v -and $v.FileSystemLabel -eq $Label) { $_ }
  }
  if ($existing) { $existing | ForEach-Object { Write-Line "found on disk $($_.DiskNumber) part $($_.PartitionNumber)" } }
  else { Write-Line 'none' }

  Write-Line ''
  Write-Line '-- boot entries --' 'Cyan'
  (& bcdedit /enum '{fwbootmgr}' 2>$null) | Select-String -Pattern 'bootsequence|displayorder|identifier' | ForEach-Object { Write-Line $_.Line.Trim() }
  $out = & bcdedit /enum all 2>$null
  $guid = $null
  foreach ($line in $out) {
    if ($line -match '^\s*identifier\s+\{([0-9a-fA-F\-]{36})\}') { $guid = $Matches[1] }
    if ($line -match '^\s*description\s+GPU Unlock \(UEFI\)\s*$' -and $guid) { Write-Line "GPU Unlock entry: {$guid}" }
  }
}

if ($Action -eq 'Inspect') {
  Write-Line ''
  Write-Line 'Next: -Action Create -DiskNumber <n> -ShrinkDrive D' 'Green'
  return
}

if ($DiskNumber -lt 0) { throw '-DiskNumber is required for Create and Remove (see -Action Inspect).' }

$reserve = ($SizeMB + $ReserveMB) * 1MB

switch ($Action) {
  'Create' {
    $source = Get-Partition -DriveLetter $ShrinkDrive
    if ($source.DiskNumber -ne $DiskNumber) {
      throw "${ShrinkDrive}: is on disk $($source.DiskNumber), not disk $DiskNumber."
    }
    $onDisk = @(Get-Partition -DiskNumber $DiskNumber)
    if ($onDisk.Count -ne 1) {
      throw ("disk {0} holds {1} partitions; this script only carves space out of a single-partition data disk." -f
        $DiskNumber, $onDisk.Count)
    }
    $supported = Get-PartitionSupportedSize -DriveLetter $ShrinkDrive
    if (($source.Size - $reserve) -lt $supported.SizeMin) {
      throw "not enough shrinkable space on ${ShrinkDrive}:."
    }
    if (Get-Partition | Where-Object { $_.GptType -eq $EspType } | ForEach-Object {
          $v = Get-Volume -Partition $_ -ErrorAction SilentlyContinue
          $v -and $v.FileSystemLabel -eq $Label } | Where-Object { $_ }) {
      throw "a $Label partition already exists; use -Action Remove first."
    }
    if (-not $DriveLetter) {
      $used = (Get-Volume | Where-Object DriveLetter | ForEach-Object { $_.DriveLetter })
      $DriveLetter = @('G', 'H', 'I', 'J', 'K', 'L') | Where-Object { $_ -notin $used } | Select-Object -First 1
      if (-not $DriveLetter) { throw 'no free drive letter found; pass -DriveLetter.' }
    }
    Write-Line ''
    Write-Line "-- create --" 'Cyan'
    Write-Line ("1. Resize-Partition -DriveLetter {0} -Size {1:N0}" -f $ShrinkDrive, ($source.Size - $reserve))
    Resize-Partition -DriveLetter $ShrinkDrive -Size ($source.Size - $reserve)
    Write-Line '   ok'
    $new = New-Partition -DiskNumber $DiskNumber -Size ($SizeMB * 1MB)
    Write-Line "2. new partition $($new.PartitionNumber) at offset $($new.Offset), $($new.Size) bytes"
    Set-Partition -DiskNumber $DiskNumber -PartitionNumber $new.PartitionNumber -GptType $EspType
    Write-Line "3. type set to EF00 (EFI System Partition)"
    $fresh = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $new.PartitionNumber
    $volume = Format-Volume -Partition $fresh -FileSystem FAT32 -NewFileSystemLabel $Label -Confirm:$false
    Write-Line "4. formatted FAT32, label $Label, $($volume.Size) bytes"
    Set-Partition -DiskNumber $DiskNumber -PartitionNumber $new.PartitionNumber -NewDriveLetter $DriveLetter
    Write-Line "5. mounted at ${DriveLetter}:" 'Green'
    $final = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $new.PartitionNumber
    Write-Line ("   volume GUID: {0}" -f $final.AccessPaths -join ' ; ')
    Write-Line ''
    Write-Line "Installed size check: $(Get-Volume -DriveLetter $DriveLetter | Select-Object -ExpandProperty SizeRemaining) bytes free" 'Green'
  }
  'Remove' {
    $target = Get-Partition -DiskNumber $DiskNumber | Where-Object {
      $_.GptType -eq $EspType -and (Get-Volume -Partition $_ -ErrorAction SilentlyContinue).FileSystemLabel -eq $Label
    }
    if (-not $target) { throw "no $Label partition on disk $DiskNumber." }
    if ($target.DriveLetter) {
      Write-Line "removing partition $($target.PartitionNumber) ($($target.DriveLetter):)" 'Yellow'
    }
    Remove-Partition -DiskNumber $DiskNumber -PartitionNumber $target.PartitionNumber -Confirm:$false
    Write-Line "deleted partition $($target.PartitionNumber)"
    $supported = Get-PartitionSupportedSize -DriveLetter $ShrinkDrive
    if ($supported.SizeMax) {
      Resize-Partition -DriveLetter $ShrinkDrive -Size $supported.SizeMax
      Write-Line "${ShrinkDrive}: grown back to $($supported.SizeMax) bytes" 'Green'
    }
  }
}

Write-Line ''
Write-Line 'done.' 'Green'
