#pragma once
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <IndustryStandard/Acpi10.h>
#include "unlock/board_profile.h"

#define PROBE_SAMPLE_COUNT 16
#define PROBE_PROM_OFFSET 0x00300000ULL
#define PROBE_PROM_SIZE 0x00100000U
#define PROBE_ROM_LIMIT 0x01000000U

/* Comparison of a live value against unlock/board_profile.h. */
#define PROBE_MISMATCH_SUBSYSTEM 0x1U
#define PROBE_MISMATCH_REVISION 0x2U
#define PROBE_MISMATCH_CLASS 0x4U
#define PROBE_MISMATCH_BDF 0x8U

typedef struct {
  UINT32 Offset;
  UINT32 Value;
  EFI_STATUS Status;
} PROBE_SAMPLE;

typedef struct {
  UINT32 Config[64];
  UINT64 BarSize;
  UINT64 BarRangeMin;
  UINT64 BarRangeMax;
  UINT64 BarGranularity;
  UINT64 BarTranslation;
  UINT8 BarResType;
  EFI_STATUS BarStatus;
  BOOLEAN MemoryEnabled;
  PROBE_SAMPLE Samples[PROBE_SAMPLE_COUNT];
} PROBE_RESULT;

/* Target selection is device + vendor + display class, never the subsystem:
   the subsystem differs per board partner and must be *reported*, not
   demanded, or the profiler would refuse to read the very card it profiles. */
BOOLEAN ProbeMatches(const UINT32 *Config);

/* Bitmask of PROBE_MISMATCH_* for the values the profile also pins. */
UINT32 ProbeProfileMismatches(const UINT32 *Config);

EFI_STATUS ProbeCollect(EFI_PCI_IO_PROTOCOL *Pci, EFI_BOOT_SERVICES *Bs,
                       PROBE_RESULT *Result);
EFI_STATUS ProbeReadProm(EFI_PCI_IO_PROTOCOL *Pci, const PROBE_RESULT *Result,
                        VOID *Buffer, UINTN Size);
BOOLEAN ProbeRomUsable(const VOID *Buffer, UINT64 Size);

/* Look up a sampled register value. Returns FALSE when it was not readable. */
BOOLEAN ProbeSample(const PROBE_RESULT *Result, UINT32 Offset, UINT32 *Value);
