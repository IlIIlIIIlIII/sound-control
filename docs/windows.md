# Windows 오디오 버전

Windows 11 x64에서 **SMSL USB DAC 스피커 EQ + MOTU M Series 입력 1 반향 제거**를 위한 네이티브 APO와 Win32 설정 앱이다. 가상 마이크/가상 출력은 생성하지 않는다. 통화 앱은 기존 `In 1-2 (MOTU M Series)` 장치를 선택한다.

## 설정 UI 디자인

Microsoft의 [Windows 11 디자인 원칙](https://learn.microsoft.com/en-us/windows/apps/design/design-principles)과 [WinUI Gallery 기반 설정 화면 지침](https://learn.microsoft.com/en-us/windows/apps/design/app-settings/guidelines-for-app-settings)을 참고했다. WinUI 3 프레임워크로 전환한 것은 아니며, 기존 Win32 앱에 Fluent 디자인 언어를 적용했다.

- 오디오 장치 → 좌우 EQ → 마이크 반향 제거 → 실제 처리 상태 순서로 구성한 카드 화면.
- Segoe UI Variable 글꼴, Segoe Fluent Icons, 둥근 카드와 스위치, 강조색의 설치 버튼.
- Windows 밝게/어둡게 설정 및 고대비 색상 반영, 모니터별 DPI 대응, 작은 창에서 세로 스크롤과 키보드 포커스 자동 노출.
- 초기 EQ 파일의 실제 필터 값을 미리 표시하고, 설치 전에는 처리 중으로 표시하지 않는다. 초기화 전 가져오기와 효과 스위치는 비활성 상태다.
- 표준 버튼·체크박스·콤보박스의 키보드 조작을 유지한다. 장식용 제목/설명은 직접 그리므로 전체 화면의 스크린 리더 검증은 별도로 필요하다.

## 빌드

Visual Studio 2022의 **Desktop development with C++**, Windows 11 SDK (22000 이상), CMake 3.26 이상이 필요하다. WDK는 향후 서명된 드라이버 패키지 배포/HLK 검증 시 추가로 필요하며, 현재 로컬 APO 빌드는 WDK의 정적 base-APO 라이브러리에 의존하지 않는다.

```powershell
.\Scripts\build-windows.ps1
```

또는:

```powershell
cmake -S . -B build\windows-msvc -A x64
cmake --build build\windows-msvc --config Release
ctest --test-dir build\windows-msvc -C Release --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File Tests\WindowsInstallerTests.ps1
cmake --install build\windows-msvc --config Release --prefix build\windows-msvc\package
```

결과물은 `MacToolsWindows.exe`, `MacToolsAPO.dll`, 설치/제거 PowerShell 스크립트다. 빌드와 테스트는 시스템 오디오에 APO를 등록하지 않는다. Windows CI는 MSVC 빌드·테스트·서명 전 패키징을 수행한다.

## 현재 PC 초기 설정

설정 창에서 다음을 선택한다.

- 출력: `스피커 (SMSL USB DAC)`
- 입력: `In 1-2 (MOTU M Series)` — Loopback/Loopback Mix가 아님
- 초기 왼쪽 EQ: `E:\Speaker\L.txt`
- 초기 오른쪽 EQ: `E:\Speaker\R.txt`

선택은 endpoint ID로 저장된다. 파일은 검증 후 `%ProgramData%\MacTools\L.txt`, `R.txt`로 복사되므로 이후 E: 드라이브가 없어도 동작한다. 최초 설치에서 위 파일이 없으면 초기화를 중단하며, 원래 오디오 효과 설정을 복원한다. 이후 설정 창에서 다른 REW 파일을 가져올 수 있다.

L: 66Hz/−12dB/Q7.40, 140Hz/−12dB/Q3.21.
R: 65Hz/−8.4dB/Q8.00, 142Hz/−12dB/Q3.36.

설정과 최근 64개 상태 변경 기록은 `%ProgramData%\MacTools`에 저장된다. 오디오 샘플은 기록하지 않는다. 설정 앱을 닫으면 트레이에 남고, 앱을 완전히 종료해도 이미 설치된 APO 효과는 유지된다.

## 설치 전 확인과 제한

**서명 전 빌드는 실제 장치에 설치하지 않는다.** 설치 도구는 DLL/EXE의 신뢰된 Authenticode 서명을 검사한다. 유효한 서명도 Protected Audio 로딩을 보장하지 않으므로 실제 오디오 처리 확인이 별도로 필요하다. Protected Audio가 요구하는 서명/패키지 조건이 충족되지 않으면 해당 조건을 해결해야 한다. `DisableProtectedAudioDG`, Secure Boot, 드라이버 서명 검사를 자동으로 변경하지 않는다.

다음은 오디오 설정을 바꾸지 않는 사전 점검이다. 이 PC에서 관찰한 ID이며 장치 재설치 후에는 설정 창에서 다시 선택한다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-windows.ps1 `
  -RenderId '{0.0.0.00000000}.{2e6721ec-e998-4442-87d3-9f5e5f3c3172}' `
  -CaptureId '{0.0.1.00000000}.{26202c49-c793-4233-8f53-acd9dc8efa05}' `
  -CheckOnly
```

서명 조건을 충족한 패키지에서는 설정 창의 **설치 및 적용**으로 관리자 설치를 실행한다. 설치 경로는 `%ProgramFiles%\MacTools`다. 복원용 원본 레지스트리 값은 일반 사용자가 수정할 수 없는 해당 폴더에 보관한다.

로컬 설치기는 기존 APO 체인이 없는 endpoint에만 연결한다. OEM 효과가 존재하면 덮어쓰지 않고 중단한다. 레지스트리 접근이 거부되면 소유권/ACL을 강제로 변경하지 않고 복원한다. 이 도구는 특정 PC용 COM/endpoint 등록 방식이며, Microsoft 서명·HLK 인증을 받은 범용 드라이버 INF 패키지가 아니다.

설치 후 SMSL·MOTU를 재연결하거나 재부팅하고 재생·통화 앱을 다시 연다. **등록 성공은 처리 성공이 아니다.** 설정 창에서 실제 APO 콜백이 실행되는지 확인한다. 상태가 대기 중이면 오디오 정지, 효과 비활성화, ASIO/독점 모드 또는 APO 로딩 실패를 구분해 확인해야 한다. Windows 이벤트 뷰어의 CodeIntegrity/Operational 로그도 서명·로딩 실패 진단에 사용할 수 있다.

다른 장치로 변경하거나 바이너리를 업데이트하려면 기존 설치를 제거한 후 다시 설치한다. 제거는 원래 효과 등록을 복원하며, 필터 파일과 설정은 보존한다.

```powershell
# 관리자 PowerShell
powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall-windows.ps1
```

## 처리 동작

- 출력은 EFX APO에서 좌우 독립으로 처리한다. 지원 스테레오 float32 샘플레이트는 44.1/48/88.2/96/176.4/192kHz다. EQ에 별도 오디오 버퍼 지연은 없다.
- 마이크는 Windows 11 CAPX MFX APO에서 48kHz float32로 처리한다. 입력 첫 채널만 사용하고 출력이 스테레오이면 처리된 모노를 양쪽에 전달한다. 모노 출력도 지원한다.
- Windows가 제공하는 보조 스피커 참조와 마이크의 QPC 시각을 동일한 시간 단위로 변환한다. 참조 장치가 선택한 SMSL과 다르면 학습/제거를 수행하지 않는다. **Windows 기본 재생 장치 및 통화 앱의 재생 장치도 SMSL로 맞춘다.**
- 가변 콜백 길이를 256프레임 AEC 블록으로 연결한다. 참조 콜백의 도착 차이를 흡수하기 위해 **고정 1024프레임(21.33ms)** 마이크 지연을 추가한다. APO `GetLatency`가 이를 보고한다. AEC를 끄더라도 스트림 시간축을 유지하기 위해 동일한 지연으로 원음을 전달한다.
- 참조 누락은 지연된 마이크 원음으로 통과한다. 큐 초과·타임스탬프 불연속·참조 장치 변경은 학습 상태를 초기화한다. 입력 과부하 및 동시 발화 보호는 기존 AEC를 유지한다.
- 설정 변경은 버전이 있는 고정 크기 공유 상태로 전달된다. 필터 해석과 계수 생성은 UI에서 수행하고, 콜백에는 사전 할당된 연산 상태만 사용한다. 처리 중 파일 접근/동적 할당/잠금 대기를 하지 않는다.
- 지원 대상은 Windows 공유 모드의 기본·통신 처리 경로다. RAW, ASIO, WASAPI 독점 모드, 시스템 오디오 효과를 끈 앱/장치는 우회할 수 있다. 물리 마이크의 펌웨어나 ASIO 입력을 수정하는 방식이 아니다.
- 장치 재연결은 Windows 장치 알림 및 오디오 엔진의 APO 재생성으로 처리한다. 앱이 MOTU 샘플레이트를 강제로 전환하거나 Windows Audio 서비스를 임의로 재시작하지 않는다. 기존 통화 앱이 끊긴 스트림을 재개하지 않는 경우 앱에서 입력 장치를 다시 열어야 한다.

## 검증 구분

자동 검증은 기존 코어/AEC 회귀, 타임스탬프·가변 패킷·큐 초과, Windows Unicode 경로 재가져오기, APO 형식 협상·바이패스·COM 수명, DLL 로딩, 설치 실패 복원을 포함한다. APO 테스트는 임시 설정 폴더에서 실행하고, 복원 테스트는 임시 HKCU 키만 사용한다.

실제 방의 반향 감소, SMSL·MOTU 드라이버의 APO 수용 여부, audiodg의 서명 검사 통과, 실제 통화 앱과의 호환성은 **서명된 빌드를 설치한 후 별도 검증해야 한다**. 합성 테스트 결과를 실제 장치 성능으로 간주하지 않는다.

개발 참고: [Windows 11 AEC 인터페이스](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-11-apis-for-audio-processing-objects), [APO 구현·설치](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects).
