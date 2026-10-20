#pragma once
/*
 * Board profile for the RTX 3090 Ti / Windows UEFI port.
 *
 * Every expectation in this port that depends on the exact card, on what the
 * motherboard firmware assigned, or on the display topology lives in this one
 * file. Nothing else in unlock/ should hardcode a bus number, a BAR address, a
 * fuse value or a ROM hash.
 *
 * Filled from a read-only profiler run on the target machine
 * (build/TI_PROFILE.EFI -> \RTX3090TI-PROBE\RUN00\REPORT.TXT,
 * 2026-10-20, Secure Boot off, cold boot, no GPU writes). Values marked
 * MEASURED reproduce that report exactly.
 */

/* ---------------------------------------------------------------- identity */
/* GA102 device IDs: 0x2203 = RTX 3090 Ti, 0x2204 = RTX 3090,
 *                   0x2206 = RTX 3080 10GB, 0x2208 = RTX 3080 Ti.
 * Same identity change the validated 3090 Ti Linux module makes. */
#define GU_PROFILE_PCI_DEVICE 0x2203U
#define GU_BOARD_ID (((UINT32)GU_PROFILE_PCI_DEVICE << 16) | 0x10deU)

/* config[0x2c] as a dword: (subsystem device << 16) | subsystem vendor.
 * MEASURED 0x50911462 (1462:5091, MSI). */
#define GU_BOARD_SUBSYSTEM 0x50911462U

/* PCI revision id, config[0x08] low byte. MEASURED 0xa1. */
#define GU_BOARD_REVISION 0xa1U

/* PMC_BOOT_0, the dword at BAR0 offset 0. MEASURED 0xb72000a1, which is the
 * same GA102 boot id the authored RTX 3080 reported. */
#define GU_BOARD_CHIP 0xb72000a1U

/* ---------------------------------------------------------------- location */
/* Where this firmware placed the card. MEASURED 0000:06:00.0. */
#define GU_BOARD_BUS 6U
#define GU_BOARD_DEVICE 0U
#define GU_BOARD_FUNCTION 0U

/* Device number of the PCIe root port that precedes the GPU in its device
 * path. MEASURED: the path is ACPI(PNP0A03,0) -> PCI(dev 3, fn 1) -> PCI(dev 0,
 * fn 0) -> END, i.e. the x16 root port at 00:03.1. Used only by the display-path
 * classifier, which matches the device number and ignores the port's function
 * (an AMD root port lives on function 1, unlike the Intel bridge the authored
 * machine used). */
#define GU_BOARD_PATH_PORT_DEVICE 3U

/* --------------------------------------------------------------------- BAR0 */
/* 0xffffffff = "accept whatever address this firmware assigned". BAR0's address
 * is motherboard-assigned, so pinning it would only add a failure mode. The
 * descriptor shape is still fully enforced (16 MiB, 32-bit memory,
 * non-prefetchable, exact structure length, end tag, checksum) and the observed
 * address is logged. MEASURED base on this boot: 0xfb000000, size 0x1000000. */
#define GU_BOARD_BAR0_ANY 0xffffffffU
#define GU_BOARD_BAR0_BASE GU_BOARD_BAR0_ANY
#define GU_BOARD_BAR0_SIZE 0x01000000U

/* The MSI-X capability dword at config[0xc8]. "The capability chain looks
 * exactly as audited" check. MEASURED 0x00050011, identical to the authored
 * RTX 3080, so it stays pinned. Set to GU_BOARD_CFG_C8_ANY to skip it. */
#define GU_BOARD_CFG_C8 0x00050011U
#define GU_BOARD_CFG_C8_ANY 0xffffffffU

/* --------------------------------------------------------------- cold state */
/* Expected values immediately after a cold power-on, before any driver has
 * run. All MEASURED on this card. */
#define GU_BOARD_COLD_PLM 0xffffff8fU     /* FEAT_PLM  0x00823804 */
#define GU_BOARD_COLD_SS0 0x67406116U     /* FEAT_SS0  0x0082381c */
#define GU_BOARD_COLD_SS1 0x00000006U     /* FEAT_SS1  0x00823820 */
#define GU_BOARD_COLD_1183A4 0x00005ff4U  /*           0x001183a4 */

/* 0x1fa828 (WPR2_HI) must be at or below this. The upstream rule, straight
 * from the validated Linux module, is "refuse when (wpr2_hi >> 4) != 0",
 * i.e. the card was already initialised since power on. MEASURED 0x00000000. */
#define GU_BOARD_COLD_WPR2_MAX 0x0000000fU

/* 0 = accept any readable SS0/SS1 (what the validated 3090 Ti Linux module
 *     does: it only rejects an inaccessible register, then verifies the written
 *     values afterwards). This card's SS1 cold value is 0x6, while the authored
 *     RTX 3080 saw 0x2 or 0x3, so the exact 3080 value is not a per-SKU
 *     invariant.
 * 1 = additionally refuse unless SS0/SS1/0x1183a4 match the MEASURED values
 *     above exactly. Stricter, and fails closed if those values vary between
 *     cold boots on this card. */
#define GU_BOARD_COLD_STRICT 0U

/* ---------------------------------------------------------------- board ROM */
/* SHA-256 of the first 1 MiB of the live VBIOS image (BAR0 + 0x300000).
 * MEASURED 52d6246160f2d0721af5f203ba47ac0323381a35517799aff2d507d30f6f502b
 * and independently reproduced from the exported PROM.BIN. */
#define GU_BOARD_PROM_SHA256 "52d6246160f2d0721af5f203ba47ac0323381a35517799aff2d507d30f6f502b"

/* ----------------------------------------------------------------- display */
/* 0 = the authored configuration: the console lives on another adapter (iGPU
 *     or a second GPU) and the target card must NOT be the display.
 * 1 = single-GPU machine: the target card is also the console display. The
 *     screen blanks between the FLR and Windows taking over. The unlock itself
 *     is unchanged, but the console-path assumption of the original port is
 *     gone, so this configuration is unverified.
 * This machine: single RTX 3090 Ti, no iGPU -> 1. */
#define GU_PROFILE_ALLOW_TARGET_DISPLAY 1

/* 0 = the authored behaviour: the device driver that owns the target card may
 *     only be released if it is not the console (the authored machine had the
 *     console elsewhere), and the display is revalidated afterwards.
 * 1 = the console display IS the target card, so the single driver that can be
 *     released before the FLR is the firmware's own display driver. That is the
 *     UEFI equivalent of the validated Linux step
 *     (aperture_remove_conflicting_pci_devices in unlock_610.c: "FLR resets the
 *     display engine, so a firmware framebuffer on this card must go first").
 *     Consequences, both deliberate: screen output stops at the release, and the
 *     post-release display revalidation is skipped. This machine: 1. */
#define GU_PROFILE_RELEASE_CONSOLE_DRIVER 1

/* ------------------------------------------------------------ automatic run */
/* 0 = the authored behaviour: the firmware entry is permanent, but Windows arms
 *     it for exactly one boot with
 *         bcdedit /set {fwbootmgr} bootsequence {<entry>}
 *     and the operator confirms the attempt with U.
 * 1 = the entry is first in {fwbootmgr} displayorder, so this app runs on every
 *     boot: it counts down GU_PROFILE_AUTO_ARM_SECONDS and then starts, unless a
 *     key is pressed. Any key during the countdown returns to the firmware, which
 *     boots Windows with the card untouched.
 *     Still required: a cold boot (a warm boot stops at stage=cold-state). After
 *     an attempt that did not finish, an UNFINISHED.TXT marker on the payload
 *     volume keeps automatic start off until U succeeds once, so a bad state can
 *     never turn into an unattended boot loop. This machine: 1. */
#define GU_PROFILE_AUTO_ARM 1
#define GU_PROFILE_AUTO_ARM_SECONDS 5U

/* ------------------------------------------------------------------ loader */
/* upstream only starts from a USB-loaded image. Set to 1 to also accept the
   machine's own EFI System Partition, so no USB stick is needed. The marker
   directory and its exact name are still required on that volume, so the app
   cannot be started from an arbitrary location. */
#define GU_PROFILE_ALLOW_INTERNAL_ESP 1

/* -------------------------------------------------------------------- labels */
#define GU_PROFILE_NAME "RTX 3090 Ti (GA102-350)"
#define GU_PROFILE_PAYLOAD_DIR L"\\RTX3090TI-UNLOCK"
#define GU_PROFILE_PAYLOAD_DIR_ASCII "RTX3090TI-UNLOCK"
#define GU_PROFILE_PROBE_DIR L"\\RTX3090TI-PROBE"
