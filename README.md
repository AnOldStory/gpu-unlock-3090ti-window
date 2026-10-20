# RTX 3090 Ti Unlock — Windows / UEFI (자동 재적용)

GeForce RTX 3090 Ti 한 장짜리 Windows 11 PC에서 **텐서(FP16/BF16) 처리율 제한을
UEFI 단계에서 해제**하는 실험입니다. USB 없이 내부 볼륨에서 실행되고, **매 콜드 부팅마다
자동으로 재적용**되며, 원하지 않으면 부팅 중 키 한 번으로 건너뜁니다.

이 저장소는 [rough2515-hub/rtx3080-unlock-public](https://github.com/rough2515-hub/rtx3080-unlock-public)
(RTX 3080 Windows/UEFI 포트)를 **RTX 3090 Ti 단일 GPU** 구성으로 옮긴 포트입니다.
언락 시퀀스는 [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock) 그대로이고,
카드/보드 종속 값만 [`unlock/board_profile.h`](unlock/board_profile.h) 한 파일로 모았습니다.

> English: a Windows/UEFI port of the RTX 3080 unlock to a single **RTX 3090 Ti**
> (GA102-350), verified on real hardware: FP16 74.08 → **138.80** TFLOPS,
> BF16 74.67 → **140.03** TFLOPS (+87%). No USB stick, the payload runs from an
> internal FAT32 (EF00) volume and re-applies on every cold boot, skippable with
> any key during a 5 s countdown.

## 측정 결과 (같은 카드, 4096³ GEMM, 중앙값 20회)

| | 잠금 (콜드 부팅 직후) | 언락 후 | 변화 |
| --- | ---: | ---: | ---: |
| FP16 Tensor | 74.08 TFLOPS | **138.80** | **+87.4%** |
| BF16 Tensor | 74.67 TFLOPS | **140.03** | **+87.6%** |
| FP32 pedantic | 26.41 TFLOPS | 27.97 | 변화 없음 |

FP32가 그대로인데 텐서 경로만 1.87배입니다. 즉 클럭 상승이 아니라
`SS0=0x88888888` / `SS1=0x8` selector의 효과입니다. 측정 환경은
torch 2.6.0+cu124 (CUDA 12.4), NVIDIA 드라이버 610.88, 메모리 10701 MHz입니다.

실제 UEFI 실행 로그는 [`evidence/RUN02-REPORT.TXT`](evidence/RUN02-REPORT.TXT)
(거래 + 화면 복구 + Windows 인계 성공), 실패 사례는
[`evidence/RUN01-REPORT.TXT`](evidence/RUN01-REPORT.TXT)에 있습니다.

### 검증 상태

- 수동(U) 경로: RUN00(진단) → RUN01(거래 성공·인계 실패) → **RUN02(전 구간 성공)**.
- 자동 모드: RUN02와 동일한 경로에 5초 카운트다운과 `UNFINISHED.TXT` 마커만 더한
  코드입니다. 이 저장소를 올린 시점 기준으로는 **다음 콜드 부팅에서 확인 예정**입니다.

## 동작 방식

1. 부팅 항목이 `{fwbootmgr}`의 `displayorder` 맨 앞에 있어, **매 부팅** 이 EFI 앱이 먼저 돕니다.
2. 앱이 카드/보드/PROM/자산을 모두 검사하고 **콜드 상태**인지 확인합니다.
3. 5초 카운트다운 → 시작하면 화면을 소유한 펌웨어 디스플레이 드라이버를 잠시 해제하고
   (Linux 쪽 `aperture_remove_conflicting_pci_devices`와 같은 역할) 카드를 독점한 뒤
   selector를 씁니다.
4. PCIe FLR로 GPU를 재설정하고, **해제했던 드라이버를 다시 연결**합니다.
   드라이버가 재초기화된 뒤에도 `SS0/SS1`이 그대로인지 보고서에 남기고,
   화면이 살아나면 Windows로 인계합니다.

언락은 **휘발성**입니다. 전원을 끄면 사라지므로 매 콜드 부팅마다 다시 적용합니다.
(그래서 자동 모드가 기본값입니다.)

## 사용법

### 한 번만 준비

1. **빌드** (Linux/WSL, Visual Studio 불필요):
   ```bash
   bash unlock/build-wsl.sh
   ```
   Windows 네이티브 빌드는 `unlock\build.ps1`입니다.
2. **자산 5개 생성** — NVIDIA 610.43.03 입력에서 만듭니다(저장소에 없음, 아래 참고).
   `unlock/assets.json`의 SHA-256과 일치해야 합니다.
3. **payload 볼륨** 준비: FAT32 256 MB, 파티션 타입 **EF00**.
   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\make_payload_volume.ps1 -Action Create -DiskNumber 1 -ShrinkDrive D
   ```
4. **BCD 항목 + 자동 모드** 설치:
   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\install_esp_entry.ps1 -Action Install -Variant unlock -Drive G: -Esp -OneTime
   # 자동(매 부팅) 실행으로 전환: 항목을 맨 앞으로, 일회성 변수는 제거
   bcdedit /set {fwbootmgr} displayorder {<entry-guid>} /addfirst
   bcdedit /deletevalue {fwbootmgr} bootsequence
   ```

   BIOS에서 **Secure Boot를 끄고**, BitLocker를 쓴다면 복구 키를 확인하세요.

### 매 부팅

- 완전 종료 후 전원 버튼으로 켜면 `READY`와 `AUTO: starting in 5 s`가 뜹니다.
- 가만히 두면 적용, **아무 키나 누르면 그 부팅은 건너뜁니다**(카드는 손대지 않음).
- 재시작(웜 부팅)에서는 `stage=cold-state`에서 멈추고 그대로 Windows로 넘어갑니다.

### 안전장치

- **실패한 상태를 무인으로 반복하지 않습니다.** 거래가 중간에 끝나지 못하면 payload
  볼륨에 `UNFINISHED.TXT`가 남고, 그 다음 부팅부터는 **U를 직접 눌러야** 시작합니다.
  한 번 성공하면 파일이 지워져 자동으로 돌아옵니다.
- 정리가 증명되지 않는 실패에서는 앱이 `STOP`을 띄우고 리소스를 붙든 채 멈춥니다.
  그때는 **전원 버튼으로 끄세요.** (언락은 휘발성이라 영구 손상은 없습니다.)
- 결과는 항상 `G:\RTX3090TI-UNLOCK\RUNnn\REPORT.TXT`에 남습니다.

### 끄기 / 되돌리기

```powershell
# 자동 실행 해제 (Windows Boot Manager를 다시 맨 앞으로)
bcdedit /set {fwbootmgr} displayorder {bootmgr} /addfirst
# payload 볼륨과 BCD 항목 제거, D: 원복
powershell -ExecutionPolicy Bypass -File tools\make_payload_volume.ps1 -Action Remove -DiskNumber 1 -ShrinkDrive D
powershell -ExecutionPolicy Bypass -File tools\install_esp_entry.ps1 -Action Remove
```

코드 수준에서 자동 모드를 끄려면 `unlock/board_profile.h`의
`GU_PROFILE_AUTO_ARM`을 `0`으로 두고 다시 빌드하면 upstream과 같은 **U 수동 실행**이 됩니다.

## 확인한 환경

- **MSI RTX 3090 Ti** (GA102-350): PCI `10de:2203`, subsystem `1462:5091`,
  VBIOS `94.02.A0.00.12` — 단일 GPU, iGPU 없음(대상 카드가 곧 화면).
- Gigabyte B550M AORUS PRO-P, Ryzen 7 5700X3D, Windows 11 Pro (UEFI).
- NVIDIA 610.88 드라이버, torch 2.6.0+cu124.
- RUN00(진단) → RUN01(거래 성공·인계 실패) → **RUN02(전 구간 성공)**.

같은 제품명이라도 다른 PC에서의 동작은 확인되지 않았습니다. 다른 카드로 옮길 때는
`board_profile.h`의 값을 그 카드에서 다시 측정해 채워야 합니다
([절차](docs/RTX3090TI-WINDOWS.md)).

## 저장소에 없는 것

- **펌웨어 자산 5개**(합계 84,427,316 bytes): NVIDIA 펌웨어에서 생성하는 산출물이라
  재배포하지 않습니다. `firmware/fetch_artifacts.py` + `generate_v67.py` +
  `extract_boot_assets.py`로 직접 만들고 `unlock/assets.json`과 대조하세요.
- **VBIOS 덤프**: NVIDIA 저작물이라 저장소 밖(`gpu-unlock-3090ti-private/`)에만 보관합니다.
  공개하는 보고서에는 카드 시리얼·GPU UUID가 없고, 부팅 볼륨의 장치 경로 고유값
  (NVMe EUI-64, 파티션 GUID)만 0으로 지웠습니다.

## 빌드 / 검사

```bash
bash unlock/build-wsl.sh                                  # EFI 앱 두 개 빌드 + PE 검사
bash unlock/test-replay-wsl.sh evidence/RUN00-REPORT.TXT  # 보고서 재생 검사
```

Windows: `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\unlock\build.ps1`.
`unlock/test-all.ps1`의 호스트 테스트는 3080 값으로 고정되어 있어 이 포트에서는
그대로 통과하지 않습니다.

## 출처와 라이선스

- 이 포트의 기반: [rough2515-hub/rtx3080-unlock-public](https://github.com/rough2515-hub/rtx3080-unlock-public)
  (GPL-2.0-only, RTX 3080 Windows/UEFI 포트)
- 언락 시퀀스 원본: [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock)
  (GPL-2.0-only, Linux 커널 모듈)
- 3090 Ti 대응의 참고: [applepines/gpu-unlock `experimental-rtx3090ti`](https://github.com/applepines/gpu-unlock/tree/experimental-rtx3090ti)
  — 장치 ID만 바꾸고 시퀀스는 그대로 둔다는 원칙을 그대로 따랐습니다.

이 프로젝트는 **GPL-2.0-only**입니다. 포함된 EDK2 헤더와 NVIDIA 고지는 각각의
라이선스를 따릅니다. [LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 참고.

## 만든 과정

- 방향 결정과 실제 하드웨어 검증(부팅·측정·전원 차단 판단)은 사용자가 직접 했습니다.
- 코드 이식, 값 측정·대조, 문서화는 **OpenCode 에이전트 + DeepSeek V4.1 Flash**가
  수행했습니다. (측정 수치는 모두 이 PC에서 실제 실행해 얻은 값이며, 위 로그로 확인할 수 있습니다.)
- 3080용 upstream 코드와 그 이전 작업은 위 출처의 저자들이 만들었습니다.

## 라이선스와 법적 고지

- **라이선스 준수**: 이 저장소는 GPL-2.0-only입니다. 원본(Zagitta/gpu-unlock)과 직접
  기반(rough2515-hub/rtx3080-unlock-public)의 저작권 고지와 라이선스를 유지했고,
  수정한 파일에는 수정 날짜를 표기했습니다([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md),
  전체 diff: [docs/3090ti-changes.diff](docs/3090ti-changes.diff)). EDK2 헤더는
  BSD-2-Clause-Patent를 그대로 따르고, 바이너리는 링크하지 않습니다.
- **NVIDIA 저작물 재배포 없음**: 펌웨어 자산 5개와 VBIOS 덤프는 저장소에 넣지 않았습니다
  (`.gitignore`). 이는 재배포 허가가 없는 파일이라, 공개 저장소에서 가장 중요한 부분입니다.
- **회색지대**: 자기 소유 하드웨어의 성능 제한을 푸는 행위는 나라에 따라 기술적 보호조치
  해제(DMCA §1201, 국내 저작권법의 기술적 보호조치 조항)나 제조사 EULA·보증 조항과
  충돌할 수 있습니다. 이 저장소는 학습·연구 목적의 실험 코드이며, 그 판단과 사용은
  사용자 책임입니다. **저작권 침해는 없습니다**(어떤 코드도 재배포 허가 위반으로
  포함되지 않았습니다).
- **보증 없음**: AS-IS. 언락은 휘발성이고 vBIOS를 영구히 바꾸지 않지만, 적용 결과에
  대한 보증은 하지 않습니다.
- **상표**: NVIDIA, GeForce, RTX는 NVIDIA Corporation의 상표입니다. 이 프로젝트는
  NVIDIA·MSI 등 제조사와 **아무런 제휴나 승인이 없으며**, 이름은 대상 하드웨어를
  설명하기 위해서만 사용합니다.

## 주의

이 저장소는 특정 한 대의 PC에서 검증한 실험입니다. GPU 펌웨어/PCIe 설정을 직접 다루므로
이론적으로는 시스템이 부팅되지 않을 수 있습니다. 언락은 전원을 끄면 사라지고 vBIOS를
영구히 바꾸지 않지만, **적용은 자기 책임**입니다. 게임 성능이나 모든 AI 워크로드의
향상률, 장시간 안정성을 의미하지 않습니다.

## 원본(3080) 문서

upstream v0.10의 3080 기준 설명, USB 부팅 절차, 측정 자료는 그대로 남아 있습니다:
[README-3080.md](README-3080.md), [docs/USB-BOOT.md](docs/USB-BOOT.md),
[results/benchmark.json](results/benchmark.json), [TESTING.md](TESTING.md).
