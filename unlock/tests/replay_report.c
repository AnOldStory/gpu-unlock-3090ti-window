/*
 * Host-side replay of a profiler REPORT.TXT through the real checks.c code.
 *
 * This does not touch hardware. It reads the dump the read-only profiler wrote
 * on the target machine and asserts that:
 *   - GuCheckConfig() accepts the captured 256-byte configuration and derives
 *     the PM/PCIe/MSI/MSI-X capability pointers from it;
 *   - GuDisplayPath() classifies both captured device paths (the GPU's own GOP
 *     handle and the PCI handle) as the target route;
 *   - the captured registers match unlock/board_profile.h;
 *   - truncations and mutations of those paths are still rejected.
 *
 * Build (from the repository root):
 *   clang -std=c11 -O2 -Wall -Wextra -fshort-wchar \
 *     -I deps/edk2/MdePkg/Include -I deps/edk2/MdePkg/Include/X64 \
 *     unlock/tests/replay_report.c unlock/checks.c -o build/replay_report
 *   build/replay_report evidence/RUN00-REPORT.TXT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checks.h"

#define MAX_NODES 16

static UINT32 Config[64];
static UINT8 GopPath[512];
static UINTN GopBytes;
static UINT8 TargetPath[512];
static UINTN TargetBytes;
static UINT32 Chip, Plm, Ss0, Ss1, Wpr2Hi, R1183a4;
static UINTN Bus = 0, Device = 0, Function = 0, Segment = 0;
static UINTN Failures;

static void Fail(const char *What, const char *Detail) {
  printf("  FAIL  %-46s %s\n", What, Detail);
  Failures++;
}

static void Ok(const char *What, const char *Detail) {
  printf("  ok    %-46s %s\n", What, Detail);
}

static UINT64 HexU64(const char *Text) { return strtoull(Text, NULL, 16); }

/* node= lines may repeat; hex is appended to Path until the terminator. */
static void ParseNodes(const char *Line, UINT8 *Path, UINTN *Used) {
  const char *Hex = Line;
  while (*Hex && *Hex != '=') Hex++;
  if (*Hex != '=') return;
  Hex++;
  UINTN Bytes = strlen(Hex) / 2;
  if (*Used + Bytes > 512) return;
  for (UINTN I = 0; I < Bytes; I++) {
    char Pair[3] = {Hex[I * 2], Hex[I * 2 + 1], 0};
    Path[(*Used)++] = (UINT8)strtoul(Pair, NULL, 16);
  }
}

static UINT32 U32(const char *Line) {
  const char *Eq = strchr(Line, '=');
  return Eq ? (UINT32)HexU64(Eq + 1) : 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: replay_report REPORT.TXT\n");
    return 2;
  }
  FILE *File = fopen(argv[1], "r");
  if (!File) {
    fprintf(stderr, "replay_report: cannot open %s\n", argv[1]);
    return 2;
  }

  char Line[2048];
  int Section = 0; /* 1 = GOP, 2 = TARGET */
  int ExpectOffset = -1;
  int Seen = 0;
  while (fgets(Line, sizeof(Line), File)) {
    Line[strcspn(Line, "\r\n")] = 0;
    if (!strncmp(Line, "[GOP]", 5)) { Section = 1; continue; }
    if (!strncmp(Line, "[TARGET]", 8)) { Section = 2; continue; }
    if (!strncmp(Line, "[", 1)) Section = 0;

    if (!strncmp(Line, "config_offset=", 14)) {
      ExpectOffset = (int)(U32(Line) / 4);
      continue;
    }
    if (!strncmp(Line, "value=", 6) && ExpectOffset >= 0 && ExpectOffset < 64) {
      Config[ExpectOffset] = U32(Line);
      ExpectOffset = -1;
      continue;
    }
    if (!strncmp(Line, "path_node=", 10)) {
      if (Section == 1) ParseNodes(Line, GopPath, &GopBytes);
      else if (Section == 2) ParseNodes(Line, TargetPath, &TargetBytes);
      continue;
    }
    if (!strncmp(Line, "segment=", 8)) { if (!Seen) Segment = U32(Line); continue; }
    if (!strncmp(Line, "bus=", 4)) { if (!Seen) Bus = U32(Line); continue; }
    if (!strncmp(Line, "device=", 7)) { if (!Seen) Device = U32(Line); continue; }
    if (!strncmp(Line, "function=", 9)) { if (!Seen) Function = U32(Line); continue; }
    if (!strncmp(Line, "identity=", 9)) { Seen = U32(Line) == GU_BOARD_ID; continue; }
  }
  fclose(File);

  /* Registers: "register=<off>" followed by "value=<v>". */
  File = fopen(argv[1], "r");
  if (!File) return 2;
  int PendingReg = -1;
  while (fgets(Line, sizeof(Line), File)) {
    Line[strcspn(Line, "\r\n")] = 0;
    if (!strncmp(Line, "register=", 9)) { PendingReg = (int)U32(Line); continue; }
    if (!strncmp(Line, "value=", 6) && PendingReg >= 0) {
      UINT32 Value = U32(Line);
      switch (PendingReg) {
        case 0x000000: Chip = Value; break;
        case 0x1fa828: Wpr2Hi = Value; break;
        case 0x823804: Plm = Value; break;
        case 0x82381c: Ss0 = Value; break;
        case 0x823820: Ss1 = Value; break;
        case 0x1183a4: R1183a4 = Value; break;
        default: break;
      }
      PendingReg = -1;
    }
  }
  fclose(File);

  printf("replay_report: %s\n\n", argv[1]);
  printf("captured: config=%s gop_path=%zu bytes target_path=%zu bytes bdf=%zu:%zu:%zu.%zu\n\n",
    Config[0] ? "yes" : "MISSING", (size_t)GopBytes, (size_t)TargetBytes,
    (size_t)Segment, (size_t)Bus, (size_t)Device, (size_t)Function);
  if (!Config[0] || !GopBytes || !TargetBytes) {
    fprintf(stderr, "replay_report: incomplete report\n");
    return 2;
  }

  printf("GuCheckConfig (captured 256-byte snapshot)\n");
  GU_CAPS Caps;
  BOOLEAN Accepted = GuCheckConfig(Config, Segment, Bus, Device, Function, &Caps);
  char Detail[160];
  snprintf(Detail, sizeof(Detail), "accepted=%u pm=%#x pcie=%#x msi=%#x msix=%#x pcie_ver=%u",
    (unsigned)Accepted, Caps.pm, Caps.pcie, Caps.msi, Caps.msix, (unsigned)Caps.pcie_version);
  if (Accepted) Ok("config gate", Detail); else Fail("config gate", Detail);

  snprintf(Detail, sizeof(Detail), "%#x == %#x", Caps.pm, 0x60U);
  if (Caps.pm == 0x60) Ok("PM capability pointer", Detail); else Fail("PM capability pointer", Detail);
  snprintf(Detail, sizeof(Detail), "%#x == %#x", Caps.pcie, 0x78U);
  if (Caps.pcie == 0x78) Ok("PCIe capability pointer", Detail); else Fail("PCIe capability pointer", Detail);
  snprintf(Detail, sizeof(Detail), "version=%u", (unsigned)Caps.pcie_version);
  if (Caps.pcie_version == 2) Ok("PCIe capability version", Detail); else Fail("PCIe capability version", Detail);

  printf("\nGuDisplayPath (captured device paths)\n");
  int GopRoute = GuDisplayPath((const EFI_DEVICE_PATH_PROTOCOL *)GopPath, GopBytes);
  int TargetRoute = GuDisplayPath((const EFI_DEVICE_PATH_PROTOCOL *)TargetPath, TargetBytes);
  snprintf(Detail, sizeof(Detail), "route=%d (want 2)", GopRoute);
  if (GopRoute == 2) Ok("GPU GOP handle path", Detail); else Fail("GPU GOP handle path", Detail);
  snprintf(Detail, sizeof(Detail), "route=%d (want 2)", TargetRoute);
  if (TargetRoute == 2) Ok("PCI handle (target) path", Detail); else Fail("PCI handle (target) path", Detail);

  /* Every truncation must still be refused. */
  UINTN Bad = 0;
  for (UINTN I = 0; I < GopBytes; I++) if (GuDisplayPath((const EFI_DEVICE_PATH_PROTOCOL *)GopPath, I) != 0) Bad++;
  for (UINTN I = 0; I < TargetBytes; I++) if (GuDisplayPath((const EFI_DEVICE_PATH_PROTOCOL *)TargetPath, I) != 0) Bad++;
  snprintf(Detail, sizeof(Detail), "%zu truncations, %zu accepted", (size_t)(GopBytes + TargetBytes), (size_t)Bad);
  if (Bad == 0) Ok("truncated paths refused", Detail); else Fail("truncated paths refused", Detail);

  /* A different port device must not be claimed as the target route. */
  UINT8 Mutated[512];
  memcpy(Mutated, TargetPath, TargetBytes);
  Mutated[5] = (UINT8)(GU_BOARD_PATH_PORT_DEVICE + 1U);
  snprintf(Detail, sizeof(Detail), "route=%d", GuDisplayPath((const EFI_DEVICE_PATH_PROTOCOL *)Mutated, TargetBytes));
  if (GuDisplayPath((const EFI_DEVICE_PATH_PROTOCOL *)Mutated, TargetBytes) == 0)
    Ok("foreign root port refused", Detail); else Fail("foreign root port refused", Detail);

  /* And neither must a target function that is not function 0. */
  memcpy(Mutated, TargetPath, TargetBytes);
  Mutated[TargetBytes - 6] = 1; /* endpoint node function byte */
  snprintf(Detail, sizeof(Detail), "route=%d", GuDisplayPath((const EFI_DEVICE_PATH_PROTOCOL *)Mutated, TargetBytes));
  if (GuDisplayPath((const EFI_DEVICE_PATH_PROTOCOL *)Mutated, TargetBytes) == 0)
    Ok("nonzero endpoint function refused", Detail); else Fail("nonzero endpoint function refused", Detail);

  printf("\nboard_profile.h expectations vs captured registers\n");
  struct { const char *Name; UINT32 Got, Want; } Cases[] = {
    {"GU_BOARD_CHIP", Chip, GU_BOARD_CHIP},
    {"GU_BOARD_COLD_PLM", Plm, GU_BOARD_COLD_PLM},
    {"GU_BOARD_COLD_SS0", Ss0, GU_BOARD_COLD_SS0},
    {"GU_BOARD_COLD_SS1", Ss1, GU_BOARD_COLD_SS1},
    {"GU_BOARD_COLD_1183A4", R1183a4, GU_BOARD_COLD_1183A4},
  };
  for (UINTN I = 0; I < sizeof(Cases) / sizeof(Cases[0]); I++) {
    snprintf(Detail, sizeof(Detail), "got %#x want %#x", Cases[I].Got, Cases[I].Want);
    if (Cases[I].Got == Cases[I].Want) Ok(Cases[I].Name, Detail); else Fail(Cases[I].Name, Detail);
  }
  snprintf(Detail, sizeof(Detail), "got %#x, limit %#x", Wpr2Hi, GU_BOARD_COLD_WPR2_MAX);
  if (Wpr2Hi <= GU_BOARD_COLD_WPR2_MAX) Ok("0x1fa828 <= GU_BOARD_COLD_WPR2_MAX", Detail);
  else Fail("0x1fa828 <= GU_BOARD_COLD_WPR2_MAX", Detail);

  snprintf(Detail, sizeof(Detail), "%#x == %#x", Config[0], GU_BOARD_ID);
  if (Config[0] == GU_BOARD_ID) Ok("GU_BOARD_ID", Detail); else Fail("GU_BOARD_ID", Detail);
  snprintf(Detail, sizeof(Detail), "%#x == %#x", Config[0x2c / 4], GU_BOARD_SUBSYSTEM);
  if (Config[0x2c / 4] == GU_BOARD_SUBSYSTEM) Ok("GU_BOARD_SUBSYSTEM", Detail); else Fail("GU_BOARD_SUBSYSTEM", Detail);
  snprintf(Detail, sizeof(Detail), "%#x == %#x", Config[0x08 / 4] & 0xffU, GU_BOARD_REVISION);
  if ((Config[0x08 / 4] & 0xffU) == GU_BOARD_REVISION) Ok("GU_BOARD_REVISION", Detail); else Fail("GU_BOARD_REVISION", Detail);
  snprintf(Detail, sizeof(Detail), "%#x == %#x", Config[0xc8 / 4], GU_BOARD_CFG_C8);
  if (Config[0xc8 / 4] == GU_BOARD_CFG_C8) Ok("GU_BOARD_CFG_C8", Detail); else Fail("GU_BOARD_CFG_C8", Detail);
  snprintf(Detail, sizeof(Detail), "%zu==%u %zu==%u %zu==%u", (size_t)Bus, GU_BOARD_BUS, (size_t)Device,
    GU_BOARD_DEVICE, (size_t)Function, GU_BOARD_FUNCTION);
  if (Bus == GU_BOARD_BUS && Device == GU_BOARD_DEVICE && Function == GU_BOARD_FUNCTION)
    Ok("GU_BOARD_BUS/DEVICE/FUNCTION", Detail); else Fail("GU_BOARD_BUS/DEVICE/FUNCTION", Detail);

  printf("\n%s: %llu failure(s)\n", Failures ? "FAIL" : "PASS", (unsigned long long)Failures);
  return Failures ? 1 : 0;
}
