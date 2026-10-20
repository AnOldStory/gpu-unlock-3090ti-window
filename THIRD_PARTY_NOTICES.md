# 외부 코드 고지

## Windows/UEFI 포트 (이 저장소의 직접 기반) — GPL-2.0-only

- 원본: [rough2515-hub/rtx3080-unlock-public](https://github.com/rough2515-hub/rtx3080-unlock-public)
- 대상: 이 저장소의 대부분(`unlock/`, `tools/`, `docs/`, 빌드 스크립트). 원본의
  Windows/UEFI 포트, payload 볼륨·BCD 도구, 보고서 재생 검사기를 그대로 이어받았습니다.
- 라이선스 원문: [LICENSE](LICENSE) (GPL-2.0-only).

2026-10-20의 로컬 변경(이 저장소): 대상을 RTX 3090 Ti 단일 GPU로 한정
(`unlock/board_profile.h`), 콘솔 디스플레이 드라이버 해제 → 독점 확보 → 거래 후 재연결
(`unlock/ownership.c`, `unlock/app.c`, `unlock/compat.c`, `unlock/port.*`),
매 부팅 자동 실행과 실패 마커(`unlock/app_io.c`, `unlock/board_profile.h`).
`docs/3090ti-changes.diff`에 upstream 대비 전체 diff가 있습니다.

## GPU unlock 코어 및 UEFI 포트 — GPL-2.0-only

- 원본: [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock/tree/0ebdb1c6308b6a2072be5bf04513472942541956)
- 고정 커밋: `0ebdb1c6308b6a2072be5bf04513472942541956`
- 대상: `unlock/vendor/`의 `unlock_610`, `prep_610`, `layout_610`, `vbios_610`, `gfw_eval`, `diagnostic.h`.
- 라이선스 원문: [LICENSE](LICENSE).

2026-10-18의 로컬 변경: MSI RTX 3080 대상 선택, UEFI의 checked I/O 및 실행·정리 상태 연결, FWSEC 진단. `tests/reference/unlock_610_v06.c`는 비교 검사에 사용하는 로컬 v0.6 스냅샷입니다. UEFI 포트와 host/mock 테스트도 GPL-2.0-only로 제공합니다. v0.10에서는 PCI I/O Attributes를 통해 Bus Master를 관리하고 프로토콜 상태와 실제 PCI Command를 함께 검사합니다.

2026-10-19의 공개용 도구·문서 정리: 로컬 USB 패키지 생성, 공개용 테스트 실행기, 상대 경로 기반 빌드 메타데이터. GPU 실행 조건과 transaction 순서는 유지했습니다.

## EDK2 헤더 — BSD-2-Clause-Patent

- 원본: [TianoCore EDK2](https://github.com/tianocore/edk2/tree/c1415771ac592ad8509351c46cd3cda4f38d7d52)
- 고정 커밋: `c1415771ac592ad8509351c46cd3cda4f38d7d52`
- 대상: `deps/edk2/MdePkg/Include/`.
- 원문과 저작권 고지: [EDK2 라이선스](LICENSES/EDK2-BSD-2-Clause-Patent.txt) 및 각 파일의 헤더.

앱은 헤더 선언을 사용하며 EDK2 라이브러리 바이너리를 링크하지 않습니다.

## 상위 프로젝트의 관련 고지

상위 프로젝트에서 보존한 NVIDIA 소스 및 EFI 참조 고지를 함께 제공합니다.

- [NVIDIA MIT 고지](LICENSES/NVIDIA-MIT.txt)
- [CMP40HX MIT 고지](LICENSES/CMP40HX-MIT.txt)
- [NVIDIA Driver License Agreement](LICENSES/NVIDIA-Driver.txt)

소스와 USB 패키지를 복사할 때 해당 라이선스와 저작권 고지를 함께 유지하세요.

## 참고한 3090 Ti 선행 작업 (코드 미포함)

- [applepines/gpu-unlock `experimental-rtx3090ti`](https://github.com/applepines/gpu-unlock/tree/experimental-rtx3090ti)
  — 3090 Ti에서도 시퀀스는 그대로 두고 장치 ID(`0x2203`)만 바꾼다는 접근을 참고했습니다.
  이 저장소에는 해당 브랜치의 코드를 복사하지 않았습니다.

## 배포하지 않는 것

- NVIDIA 펌웨어에서 생성하는 자산 5개(84,427,316 bytes): 저장소에 없고, 사용자가
  NVIDIA 배포본에서 직접 생성합니다. 파일명·크기·SHA-256만 `unlock/assets.json`에 있습니다.
- live VBIOS 덤프: NVIDIA 저작물이라 저장소 밖에만 보관하고 배포하지 않습니다.
