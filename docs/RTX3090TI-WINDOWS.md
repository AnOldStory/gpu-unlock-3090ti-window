# RTX 3090 Ti / Windows (UEFI) 포트

이 문서는 upstream `rough2515-hub/rtx3080-unlock-public` (v0.10, 커밋 `a08254d`)을
**RTX 3090 Ti 한 장짜리 Windows PC**에서 쓸 수 있게 옮긴 내용을 정리합니다.
참조한 3090 Ti 선행 작업은 `applepines/gpu-unlock` 브랜치 `experimental-rtx3090ti`이며,
거기서 3090 Ti를 위해 실제로 바뀐 것은 **장치 ID 인식 한 곳**뿐입니다
(`RTX_3090_TI 0x2203` + `supported()` + `PCI_ANY_ID` 스캔). 언락 시퀀스 자체는
3090과 동일한 GA102 코드를 그대로 씁니다.

그래서 이 포트도 같은 원칙을 지킵니다. **시퀀스는 손대지 않고, 장치 인식과
"이 보드/이 PC에서만 참인 값"만 `unlock/board_profile.h` 한 파일로 모았습니다.**

## 1. 바뀐 범위

| 파일 | +/- | 내용 |
|---|---:|---|
| `unlock/board_profile.h` | 신규 | 카드/보드/화면에 종속된 모든 기대값 |
| `unlock/port.h` | +4 / -3 | 하드코딩 3줄 → `#include "board_profile.h"` |
| `unlock/port.c` | +2 / -1 | BDF `01:00.0` → 프로파일 BDF |
| `unlock/checks.c` | +4 / -2 | BDF 검사, 디스플레이 경로 노드 |
| `unlock/app.c` | +55 / -20 | ConfigCheck/BarCheck/ColdCheck/PromCheck 값 치환, `DisplayCheck` 완화 |
| `unlock/app_io.c` | +2 / -2 | payload 폴더명, 로그 헤더 |
| `unlock/vendor/unlock_610.c` | +5 / -3 | `GU_TARGET_PCI_ID`를 프로파일에 연결 (기본값 정의 순서만) |
| `probe_core.*`, `probe_app.c` | +158 / -11 | **진단 앱**(언락 경로 아님)을 3090 Ti용으로 재조준 |

언락 앱 자체의 로직 변경은 위 `app.c`의 값 치환과 `DisplayCheck` 하나뿐입니다.

### 원본에 박혀 있던 값 (그리고 왜 문제인지)

| 위치 | 원본 값 | 이 PC |
|---|---|---|
| `checks.c` | BDF `0000:01:00.0` | `0000:06:00.0` |
| `app.c` BarCheck | BAR0 min `0x84000000`, max `0x84ffffff` | 펌웨어가 할당 |
| `app.c` ConfigCheck | `config[0x10] == 0x84000000`, `config[0xc8] == 0x00050011` | 동일하지 않음 |
| `app.c` ColdCheck | `PLM 0xffffff8f`, `SS0 0x46254040`, `0x1183a4 0x2800` | 확인 필요 |
| `app.c` PromCheck | VBIOS SHA-256 `f3dc8f07…` (MSI RTX 3080) | 다른 ROM |
| `port.h` | subsystem `1462:3890` | `1462:5091` |
| `app.c` DisplayCheck | 콘솔이 Intel iGPU, 대상 카드는 화면이면 거부 | iGPU 없음 → 거부 |

리눅스 모듈은 이 값들을 **바인딩된 장치에서 런타임에 읽기 때문에** 필요 없지만,
이 UEFI 포트는 작성자가 관찰한 값을 **사전 조건**으로 씁니다. 그래서 장치 ID 한 줄로는
통과하지 못합니다.

## 2. 빌드 (Visual Studio 불필요)

upstream `unlock/build.ps1`은 VS2019 LLVM(`clang.exe`/`lld-link.exe`)을 요구합니다.
이 포트는 WSL/Linux의 `clang` + GNU `ld`로 같은 PE32+ EFI 두 개를 만듭니다.

```bash
# WSL: Ubuntu-24.04 기준, clang-18 + binutils
cd /mnt/c/Users/<you>/gpu-unlock-3090ti
bash unlock/build-wsl.sh            # 둘 다
bash unlock/build-wsl.sh app        # 언락 앱만
bash unlock/build-wsl.sh profile    # 진단 앱만
```

산출물:

- `unlock/build/BOOTX64.EFI` — 언락 앱 (자산 5개 필요)
- `build/TI_PROFILE.EFI` — 읽기 전용 진단 앱 (자산 불필요)

두 파일 모두 `Subsystem 0000000a (EFI application)`, `ImageBase 0`으로 링크됩니다.

## 3. USB 만들기

```powershell
python tools\package_usb_3090ti.py --variant profile --output .\usb-profile
python tools\package_usb_3090ti.py --variant unlock  --assets .\assets --output .\usb-unlock
```

출력 폴더의 **내용물**을 FAT32 USB 최상위에 복사합니다. 스크립트는 드라이브를
선택·포맷하지 않습니다. `EFI/BOOT/BOOTX64.EFI`가 부팅 이미지이고,
`RTX3090TI-PROBE`(진단) / `RTX3090TI-UNLOCK`(언락) 폴더가 각각의 작업 폴더입니다.

## 3-B. USB 없이 (내부 EFI 시스템 파티션)

USB를 쓰지 않아도 됩니다. Windows가 이미 부팅에 쓰는 내부 디스크의 **EFI 시스템
파티션(ESP, FAT32)** 에 앱을 넣고 BCD에 부팅 항목 하나를 추가하면 됩니다.

```powershell
# 1) 계획만 확인 (아무것도 바꾸지 않음, 관리자 권한 불필요)
powershell -ExecutionPolicy Bypass -File tools\install_esp_entry.ps1 -Action DryRun -Variant profile -OneTime

# 2) 관리자 PowerShell에서 실제 설치 (한 번만 부팅)
powershell -ExecutionPolicy Bypass -File tools\install_esp_entry.ps1 -Action Install -Variant profile -OneTime

# 3) 되돌리기
powershell -ExecutionPolicy Bypass -File tools\install_esp_entry.ps1 -Action Remove
```

스크립트가 하는 일은 다섯 가지뿐입니다.

1. `mountvol S: /S` 로 ESP에 문자를 붙임
2. `S:\EFI\gpulock\BOOTX64.EFI` 로 앱 복사 (원래 `\EFI\Microsoft\...`는 건드리지 않음)
3. `S:\RTX3090TI-PROBE`, `S:\RTX3090TI-UNLOCK\ASSETS` 폴더 생성
4. `bcdedit /copy {bootmgr} /d "GPU Unlock (UEFI)"` + `device`/`path` 지정
5. `-OneTime`이면 `bcdedit /set {fwbootmgr} bootsequence {guid}` — UEFI `BootNext`라서
   **다음 부팅 1회만** 실행되고 그 다음에는 자동으로 Windows로 넘어갑니다.
   (참고: `bcdedit /bootsequence`는 Windows Boot Manager용이라 이 체인에는 효과가 없습니다.
   이 앱은 펌웨어가 직접 로드하기 때문입니다.)

주의:

- **Secure Boot는 USB든 ESP든 반드시 꺼야 합니다.** 서명되지 않은 EFI 앱은 아예 로드되지 않습니다.
- BCD/ESP를 수정하므로 **BitLocker 복구 키**를 미리 확인하세요. Secure Boot를 끄면 PCR7이 바뀌어
  복구 키를 요구할 수 있습니다.
- 잘못되면 `-Action Remove`로 항목과 파일을 지우면 원래대로 돌아옵니다. Windows 자체
  부팅 파일은 수정하지 않습니다.

이 경로를 쓰려면 로더 검사 한 곳이 완화되어 있어야 합니다
(`unlock/board_profile.h`의 `GU_PROFILE_ALLOW_INTERNAL_ESP 1` — 이 체크아웃은 이미 1).
USB 이미지에서는 이 플래그가 켜져 있어도 **USB 경로가 우선**이고, 마커 폴더가
없으면 여전히 거부합니다.

### 한 가지 제약: 진단은 ESP로 되지만, 언락은 안 됩니다

이 PC의 ESP는 **정확히 100 MB**입니다(디스크 0 파티션 0). Windows EFI 파일이 이미
그 안에 있어서 남는 공간은 60~70 MB 수준입니다. 반면 언락 자산 5개는 합계
**약 85 MB**(`gsp_ga10x.bin` 하나가 84 MB)라 ESP에 들어가지 않습니다.
진단 앱은 31 KB라 문제없습니다.

언락 단계에서 쓸 수 있는 방법:

| 방법 | 준비 | 비고 |
|---|---|---|
| USB | FAT32 USB 128 MB 이상 | 가장 단순. `tools/package_usb_3090ti.py` |
| 전용 FAT32 파티션 | D:에서 260 MB 축소 | `tools/make_payload_volume.ps1` (이 PC에서 사용) |

```powershell
# 관리자 PowerShell. 순서: 조사 -> 생성. 생성은 D:를 260MB 줄이고 256MB 파티션을 만듭니다.
powershell -ExecutionPolicy Bypass -File tools\make_payload_volume.ps1 -Action Inspect
powershell -ExecutionPolicy Bypass -File tools\make_payload_volume.ps1 -Action Create -DiskNumber 1 -ShrinkDrive D
# 되돌리기
powershell -ExecutionPolicy Bypass -File tools\make_payload_volume.ps1 -Action Remove -DiskNumber 1 -ShrinkDrive D
# 설치 (파티션이 G:로 잡혔다면)
powershell -ExecutionPolicy Bypass -File tools\install_esp_entry.ps1 -Action Install -Variant unlock -Drive G: -Assets .\assets -OneTime
```

새 파티션은 **EFI 시스템 파티션 타입(EF00)** 으로 만듭니다. UEFI 펌웨어는 EF00이
아니면 FAT32 파티션을 아예 파일 시스템으로 노출하지 않는 경우가 많습니다. 사양상
ESP가 두 개인 것은 허용됩니다. 파티션 타입이 EF00이면 Windows 부팅 구성도 그대로
동작합니다(BCD 항목은 파티션을 명시적으로 가리킵니다).

`Inspect`는 디스크/파티션/BitLocker/축소 가능 공간을 **읽기만** 합니다. `Create`는
D:가 **파티션 하나뿐인 디스크**에 있을 때만 동작하게 해 두었습니다(되돌리기가
깔끔해지도록). BitLocker가 켜져 있으면 복구 키를 먼저 확인하세요(이 PC는 둘 다
`FullyDecrypted`).

> 주의: 3-B/3-C의 설치 스크립트는 복사가 중간에 실패하면 조용히 넘어갈 수 있는
> 지점이 있었습니다. 지금은 5개 자산의 **존재와 크기를 마지막에 다시 검사**하고
> 실패하면 예외를 던집니다.

## 4. 1단계 — 진단 (읽기 전용, 카드에 아무것도 쓰지 않음)

1. BIOS에서 **Secure Boot 해제**. USB면 USB로 부팅(UEFI 모드), USB를 안 쓰면 3-B의
   `-Action Install -OneTime`으로 재부팅 한 번.
2. 화면에 보고가 뜨고 `\RTX3090TI-PROBE\RUN00\REPORT.TXT`가 저장됩니다
   (USB면 USB, ESP 경로면 ESP의 `S:\RTX3090TI-PROBE\`). 15초 뒤 자동으로 Windows가 부팅됩니다.
3. 관리자 PowerShell에서 보고서를 꺼내서 봅니다 (ESP를 읽기 전용으로 마운트했다가 해제):
   `powershell -ExecutionPolicy Bypass -File tools\check_status.ps1 -Esp`
   `REPORT.TXT`와 `PROM.BIN`이 `-Export` 폴더(기본
   `%TEMP%\gpu-unlock-export`)로 복사됩니다.
4. 그 REPORT에서 `[PROFILE]` 절과 `[PASTE-READY board_profile.h values]` 절을 봅니다.

채워야 하는 값은 다음뿐입니다.

```c
#define GU_BOARD_SUBSYSTEM   0x…   /* config[0x2c] */
#define GU_BOARD_REVISION    0x…   /* config[0x08] 하위 바이트 */
#define GU_BOARD_CHIP        0x…   /* BAR0[0x00] */
#define GU_BOARD_BUS/DEVICE/FUNCTION      /* target BDF */
#define GU_BOARD_CFG_C8      0x…   /* config[0xc8] (MSI-X 캡 위치) */
#define GU_BOARD_COLD_PLM / _SS0 / _1183A4
#define GU_BOARD_PROM_SHA256 "…"   /* prom_sha256 */
#define GU_BOARD_PATH_PORT_DEVICE  … /* [TARGET] path_node의 브리지 device */
```

`GU_BOARD_BAR0_BASE`와 `GU_BOARD_CFG_C8`은 `ANY`로 두면 값 고정 없이
**구조 검사**(16 MiB / 32비트 / 정확한 descriptor / end tag / checksum)만 합니다.

## 4-B. 이 PC의 RUN00 측정 결과 (완료, 2026-10-20)

2026-10-20, Secure Boot 해제, 콜드 상태, GPU 쓰기 0회. 원본 보고서는 프로브 볼륨
(`\\?\Volume{<probe-volume-guid>}\RTX3090TI-PROBE\RUN00\REPORT.TXT`)에 있고, 이 트리에도
`evidence/RUN00-REPORT.TXT`로 복사해 두었습니다. VBIOS 원본(1 MiB)은 NVIDIA 펌웨어라
저장소 밖(`gpu-unlock-3090ti-private/`)에만 보관합니다. 보고서에서 장치 경로의
NVMe EUI-64와 파티션 GUID는 공개 전에 0으로 지웠습니다(카드 시리얼·GPU UUID는 원래 없음).

| 항목 | 측정값 | 판정 |
|---|---|---|
| 실행 볼륨 | `output_volume=2` (내부 EFI 시스템 파티션) | USB 불필요 경로 확인 |
| 카드 | `0000:06:00.0`, `10de:2203`, subsys `1462:5091`, rev `a1` | 일치 |
| 매칭 카드 수 | 1 | 오탐 없음 |
| BAR0 | `0xfb000000`, 16 MiB, memory decode on | 구조 검사 통과 |
| chip (`PMC_BOOT_0`) | `0xb72000a1` | 3080과 동일 |
| `config[0xc8]` (MSI-X 캡) | `0x00050011` | 3080과 동일 |
| cold PLM | `0xffffff8f` | 3080과 동일 |
| cold SS0 / SS1 | `0x67406116` / `0x6` | 3080(`0x46254040` / `2\|3`)과 **다름** |
| cold `0x1183a4` | `0x5ff4` | 3080(`0x2800`)과 **다름** |
| WPR2_HI (`0x1fa828`) | `0` | 콜드(전원 인가 후 미초기화) |
| GFW | cpuctl halted, plm L0, boot `0xff` | 통과 |
| ROM 이미지 | 149,504 bytes, SHA-256 `52d62461…6f502b` | PROM.BIN으로 독립 재계산 일치 |
| 디스플레이 | GOP 1개 = **타깃 카드 자신**, 3840×2160 | 단일 GPU (화면 꺼짐 감수) |
| 브리지 노드 | `ACPI(PNP0A03,0) → PCI(dev 3, fn 1) → PCI(dev 0, fn 0)` | **AMD 루트포트 `00:03.1`** |

위 값은 모두 `unlock/board_profile.h`에 들어가 있습니다.

### SS0/SS1은 값으로 고정하지 않습니다

이 카드의 SS1 콜드 값은 `0x6`인데 원저자의 3080은 `0x2`/`0x3`을 허용했습니다. 즉
SS1은 SKU별 불변값이 아닙니다. 그래서 `ColdCheck`는 **검증된 3090 Ti 리눅스 모듈과
같은 의미**로 동작합니다: SS0/SS1이 *읽히는지*(`0xffffffff`가 아닌지)만 보고, 쓴
뒤의 값(`0x88888888` / `0x8`)은 시퀀스 안에서 다시 검증합니다.
측정값 완전 일치를 요구하려면 `board_profile.h`에서 `GU_BOARD_COLD_STRICT 1`.

### 오프라인 재생 테스트 (재부팅 없이 검증)

```bash
bash unlock/test-replay-wsl.sh evidence/RUN00-REPORT.TXT
```

`unlock/checks.c`를 호스트에서 컴파일해 **실제 보고서 바이트**를 그대로 넣습니다:
`GuCheckConfig()`가 256바이트 config를 받아들이고 PM=`0x60`/PCIe=`0x78`을 찾아내는지,
`GuDisplayPath()`가 GOP 경로(끝에 ACPI `_ADR` 노드가 붙은 형태)와 PCI 경로를 모두
route 2로 분류하는지, 잘린 경로와 다른 device/function은 거부하는지. 이 PC의
바이트로 18개 검사 전부 통과합니다.

## 5. 2단계 — 언락

전제:

- 자산 5개가 `RTX3090TI-UNLOCK\ASSETS\`에 있고 `unlock/assets.json`의 SHA-256과 일치
- BIOS **Secure Boot 해제**, BitLocker를 쓴다면 **복구 키 확인**
- 반드시 **콜드 부팅**(완전 전원 차단 후) 상태에서 실행

절차 (이 PC에서 실제로 세팅된 상태):

1. G:(`GPULOCK`, 256 MB FAT32)에 `EFI\gpulock\BOOTX64.EFI`와
   `RTX3090TI-UNLOCK\ASSETS\*` 5개가 있고, BCD 항목 `GPU Unlock (UEFI)`가
   `device partition=G:`, `path \EFI\gpulock\BOOTX64.EFI`를 가리킵니다.
2. `{fwbootmgr}`의 `displayorder` **맨 앞**에 이 항목이 있어 **매 부팅** 실행됩니다
   (자동 모드). 예전 방식은 `bootsequence {<entry-guid>}`로 한 번만 실행이었습니다.
   (`bcdedit /bootsequence`가 아니라 `{fwbootmgr}`이어야 합니다:
   이 체인에는 Windows Boot Manager가 관여하지 않습니다.)
3. 전원을 끄고 다시 켭니다(콜드 부팅). 사전 점검이 모두 통과하면 `READY`와 함께
   5초 카운트다운이 뜨고, 아무 키도 누르지 않으면 자동으로 시작합니다.
   중간에 멈추고 싶으면 그 5초 안에 아무 키나 누르면 됩니다.
4. 카운트다운이 끝나면 `ARMED` → 트랜잭션 →
   `UEFI transaction completed and cleaned up...` → Windows가 이어서 부팅합니다.
   **시작 직후 화면이 잠깐 멈추는 것이 정상입니다**(아래 단일 GPU 경고 참고).
   수동 모드(`GU_PROFILE_AUTO_ARM 0`)나 `UNFINISHED.TXT`가 남은 부팅에서는
   `READY`에서 **U 한 번**으로 시작합니다.
5. 쓰기 없이 점검만 확인하고 싶으면 카운트다운 중 아무 키나 누르세요.
   자산 5개 로드와 모든 점검이 `Arm()` **앞**에서 끝나므로, 이 한 번의 부팅으로
   "이 구성이 실제로 도는지"를 쓰기 없이 확인할 수 있고 보고서도 그대로 남습니다.

`READY` 전에 실패하면 앱은 아무것도 쓰지 않고 종료하고 Windows가 부팅합니다.
그때는 **키를 누르지 않아도** 아무 일도 일어나지 않습니다. `G:\RTX3090TI-UNLOCK\RUN00\REPORT.TXT`의
`FINAL status=… stage=…` 줄이 원인을 알려줍니다(예: `stage=display`,
`stage=cold-state`, `stage=assets`).

### 단일 GPU 경고 / 콘솔 드라이버 해제

이 PC에는 iGPU가 없어 **대상 카드가 곧 화면**입니다. 원본은 이 상황을 거부하도록
되어 있고, 이 포트는 `GU_PROFILE_ALLOW_TARGET_DISPLAY 1`과
`GU_PROFILE_RELEASE_CONSOLE_DRIVER 1`로 완화했습니다.

RUN00 실측에서 `stage=ownership`이 `EFI_ACCESS_DENIED`로 막혔습니다. 카드가 콘솔이라
**펌웨어 그래픽(GOP) 드라이버가 PCI I/O를 `BY_DRIVER`로 점유**하고 자식 핸들 2개를
만들었기 때문입니다. 원본은 이 경우를 의도적으로 거부합니다(콘솔이 살아 있어야 하므로).
그런데 이 포트에서 독점 소유는 선택이 아니라 필수입니다: `GuDmaAllocate()`가
`Gu.owned`를 요구하므로 GSP 펌웨어 84 MB를 VRAM으로 옮기려면 독점이 필요합니다.

그래서 원저자의 Linux 경로(`unlock_610.c:1388`: "FLR resets the display engine, so a
firmware framebuffer on this card must go first" → `aperture_remove_conflicting_pci_devices`)를
UEFI 등가물로 옮겼습니다: **U 직후 그 디스플레이 드라이버를 해제(`DisconnectController`)하고
독점을 다시 잡습니다.**

- **화면은 U를 누른 직후 멈춥니다**(FLR 시점이 아니라 해제 시점). 이후 트랜잭션 로그는
  `REPORT.TXT`에만 남습니다.
- 해제 후에는 GOP가 없으므로 `display` 재검사는 건너뜁니다
  (`display-after-release skipped: the released driver was the console`).
- 거래 중 `aperture_remove_conflicting_pci_devices()`는 `Gu.console_released`를 보고
  즉시 0을 반환합니다(콘솔은 이미 제거됨).
- 정리(cleanup)가 증명되지 않으면 앱은 `STOP`을 찍고 **전원 버튼으로 끄기 전까지
  리소스를 붙잡습니다.** 이때는 Windows로 넘어가지 마세요.
- **이 구성은 원작자가 검증한 조건이 아닙니다.** 보조 GPU/iGPU를 붙여 화면을 옮기면
  두 스위치를 0으로 두어 원래 동작으로 돌아갑니다.

**RUN01 결과 (2026-10-20 08:43)** — 거래는 성공, 핸드오프는 실패:

```
driver release: status=0 refresh=0 same_pci=1
ownership-after-release: records=6 exclusive=1 status=0
decode-after-release: command 0x3 → 0x0 → 0x3, readback matched=1
PCIe FLR returned: result=0
post-FLR SEC2 reset released, GSP halted, BME=0 ... SS0=88888888 SS1=00000008
transaction result=0 containment=0 started=1 quiesced=1 fault=0 retained=0 FLR=1
FINAL status=0 stage=transaction attempted=1 driver_release=1 clean=1
```

즉 해제 → 독점 → 복구 → FWSEC/GSP → selector 기록(`0x88888888`/`0x8`) → FLR →
정리까지 **전부 통과**했습니다. 문제는 그 다음: 디스플레이 드라이버가 없으니
**펌웨어 부트 메뉴와 Windows Boot Manager가 그릴 화면이 없어** 넘어가지 못했고,
전원을 껐다 켜야 했습니다(그래서 그 부팅의 언락은 휘발됨).

그래서 거래 성공 후 **해제했던 드라이버를 다시 연결**하도록 보완했습니다:
`ConnectController(Gu.handle, NULL, NULL, TRUE)` → `post-restore selectors: SS0=… SS1=…`
를 보고서에 남겨, 그 드라이버의 재초기화가 selector를 덮어쓰지 않는지 매번 확인합니다.

**RUN02 결과 (2026-10-20 08:54) — 성공, Windows로 정상 인계:**

```
driver release: status=0 refresh=0 same_pci=1
decode readback matched=1
pre-FLR selectors: ss0=0x88888888 expected=0x88888888 ss1=0x8 expected=0x8
post-FLR SEC2 reset released, GSP halted, BME=0 ... SS0=88888888 SS1=00000008
restore PCI command: actual=0x3 original=0x3 mask=0x6
display restore: connect=0 owned=0
post-restore selectors: SS0=0x88888888 SS1=0x8 (unlock wrote 0x88888888/0x8)
Display restored: the released driver reconnected, screen output resumes.
FINAL status=0 stage=display-restore attempted=1 driver_release=1 clean=1
```

즉 **드라이버 재초기화가 selector를 덮어쓰지 않았고**, 화면도 살아나 전원 차단 없이
Windows로 넘어갔습니다. 같은 카드에서 잰 A/B (torch 2.6.0+cu124, 4096³ GEMM, 중앙값 20회):

| | 잠금 (콜드 부팅 직후) | RUN02 이후 | 변화 |
| --- | ---: | ---: | ---: |
| FP16 Tensor | 74.08 TFLOPS | **138.80** | **+87.4%** |
| BF16 Tensor | 74.67 TFLOPS | **140.03** | **+87.6%** |
| FP32 pedantic | 26.41 TFLOPS | 27.97 | 변화 없음 |

FP32는 그대로인데 텐서 경로만 1.87배이므로, 클럭 상승이 아니라 selector 효과입니다.

### 매 부팅 자동 재적용 (언락은 휘발성)

전원을 끄면 selector가 사라집니다. 그래서 이 포트는 **매 부팅 자동 실행** 모드
(`GU_PROFILE_AUTO_ARM 1`)를 기본값으로 둡니다. 부팅 항목이 `{fwbootmgr}`의
`displayorder` **맨 앞**에 있고, `bootsequence`(일회성)는 쓰지 않습니다.

콜드 부팅하면:

1. `READY`와 함께 `AUTO: starting in 5 s` 카운트다운이 보입니다.
2. 가만히 두면 5초 뒤 거래가 시작됩니다. 화면이 잠깐 꺼졌다가 돌아오고 Windows로 넘어갑니다.
3. **아무 키나 누르면** 그 부팅은 건너뜁니다(카드는 손대지 않고 Windows로 직행).

안전장치 두 가지:

- **웜 부팅(재시작)에서는 실행되지 않습니다.** 카드가 이미 초기화된 상태라
  `stage=cold-state`에서 멈추고 그대로 Windows로 넘어갑니다. 언락은 완전 종료 후
  전원 버튼으로 켤 때만 적용됩니다.
- **거래가 중간에 끝나지 못하면** payload 볼륨에 `UNFINISHED.TXT`가 남고, 그 다음
  부팅부터는 자동 시작이 꺼져 **U를 직접 눌러야** 합니다. 즉 실패한 상태를 무인으로
  반복하지 않습니다. 한 번 성공하면 그 파일이 지워져 다시 자동으로 돌아옵니다.
  (강제로 되돌리려면 `G:\RTX3090TI-UNLOCK\UNFINISHED.TXT`를 지우면 됩니다.)

자동 모드를 끄고 예전처럼 **한 번만** 실행하려면:

```c
// unlock/board_profile.h
#define GU_PROFILE_AUTO_ARM 0
```

로 바꿔 다시 빌드·설치하고, 부팅 순서를 원래대로 되돌립니다(관리자 PowerShell):

```powershell
# 자동 실행 해제: Windows Boot Manager를 다시 맨 앞으로
bcdedit /set {fwbootmgr} displayorder {bootmgr} /addfirst
# 필요하면 일회성 실행으로: 엔트리 GUID는 bcdedit /enum {fwbootmgr} 로 확인
bcdedit /set {fwbootmgr} bootsequence {<your-entry-guid>}
```

EFI나 자산을 새로 넣었을 때만
`tools\install_esp_entry.ps1 -Action Install -Variant unlock -Drive G: -Esp -OneTime`를 쓰고,
결과 확인은 `tools\check_status.ps1 -Drive G:`입니다.

## 6. 자산 5개 만들기

upstream의 firmware 도구를 그대로 씁니다 (Zagitta/gpu-unlock `firmware/`):

```
fetch_artifacts.py    # 고정된 NVIDIA 610.43.03 입력 수집 (네트워크 필요)
generate_v67.py       # V67 페이로드 생성
extract_boot_assets.py
```

리눅스에서 `python3` + `zstd`가 필요합니다. 생성물의 SHA-256이 `unlock/assets.json`과
일치하는지 반드시 확인하세요. 이 저장소는 펌웨어를 재배포하지 않습니다.

**이 PC에서는 이미 생성 완료했습니다** (`assets/`, 합계 84,427,316 bytes). sudo 없이
`zstd`를 쓰려면 `apt-get download zstd` + `dpkg-deb -x`로 사용자 홈에 풀면 됩니다.
다섯 파일 모두 `unlock/assets.json`과 SHA-256이 일치하는 것을 확인했습니다:

```
056cbc06… booter_load_ga102.bin    61256
2a0aa1f8… v67-ga102-610.43.03.bin  64000
57237362… gsp_ga10x.bin         84277400
3df651ac… gsp_boot_ga102.bin       24576
cc5a3fc1… gsp_boot_desc_ga102.bin     84
```

## 7. 검증 상태

- 두 EFI 모두 이 PC의 WSL에서 빌드·PE 검증 완료.
- 1단계(RUN00) 진단은 **실제 하드웨어에서 완료**했고, 모든 카드별 값을
  `board_profile.h`에 채웠습니다. `evidence/RUN00-REPORT.TXT` 참고.
- 2단계 앱도 실제 하드웨어에서 실행: RUN00(사전 점검 통과, `ownership`에서 중단) →
  RUN01(**거래 성공**, 인계 실패) → **RUN02(거래 + 인계 + 화면 복구 모두 성공)**.
  보고서는 `evidence/RUN01-REPORT.TXT`, `evidence/RUN02-REPORT.TXT`.
- **효과 검증 완료**: FP16 74.08 → **138.80**, BF16 74.67 → **140.03** TFLOPS(+87%).
  FP32는 변화 없음(26.41 → 27.97).
- 현재 페이로드: `unlock/build/BOOTX64.EFI` 103,604 bytes,
  sha256 `617e6313…`(G:에 설치·해시 확인 완료).
- 이 카드에서는 남은 미검증 항목이 없습니다. 다른 카드/펌웨어로 옮길 때는
  `board_profile.h`를 다시 채우고 위 단계를 반복하면 됩니다.
- `unlock/test-replay-wsl.sh`로 실제 보고서 바이트를 `checks.c`에 재생해
  점검 로직을 오프라인으로 검증했습니다(18개 검사 통과).
- upstream의 호스트 테스트(`unlock/tests/*`)는 3080 값으로 고정되어 있어 이 포트에서는
  그대로 통과하지 않습니다. `test-all.ps1`을 돌리려면 픽스처를 3090 Ti 값으로 바꿔야 합니다.
