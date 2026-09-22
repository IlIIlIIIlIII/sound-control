# Windows 오디오 버전

Windows 11 x64에서 **SMSL USB DAC 스피커 EQ + MOTU M Series 입력 1 반향 제거**를 위한 네이티브 APO와 WinUI 3 설정 앱이다. 가상 마이크/가상 출력은 생성하지 않는다. 통화 앱은 기존 `In 1-2 (MOTU M Series)` 장치를 선택한다.

## 출력 장치 자동 복구

설치된 `PersonalTools Device Recovery` 작업은 부팅 시와 1분마다 선택된 출력의 연결 상태를 확인한다. 기존 endpoint가 활성 상태면 아무것도 변경하지 않는다. 사라졌으면 저장된 USB 시리얼과 오디오 기능 식별자를 우선 비교하고, 시리얼이 없는 장치는 Container ID, 마지막으로 USB VID/PID와 오디오 기능이 일치하는 단일 후보를 찾는다. 다른 시리얼로 대체하지 않으며, 복수 후보나 식별 정보 조회 오류가 있으면 자동 선택을 보류한다. 제품명만으로 선택하지 않는다. 시리얼 없는 장치의 단일 모델 일치는 물리적 개체의 고유성 보장은 아니다.

새 출력의 기존 효과 체인이 비어 있거나 PersonalTools 자체 효과인 경우에만 EQ를 등록한다. 필터와 EQ/AEC 켜짐 설정은 보존한다. 새 endpoint의 원래 레지스트리 값과 진행 중 복구 기록을 먼저 저장하고, 참조 출력 ID를 갱신한 다음 Windows Audio를 재시작한다. 이때 재생/통화가 잠깐 끊길 수 있고 앱에서 스트림을 다시 열어야 할 수 있다. 마이크 입력 장치의 자동 변경은 포함하지 않는다.

작업은 SYSTEM 권한으로 실행하지만 실행 코드·장치 식별 정보·복원 기록은 관리자만 쓸 수 있는 Program Files에 보관한다. 사용자 쓰기 가능한 설정에서 명령이나 경로를 받아 실행하지 않는다. 제거 시 자동 복구 작업을 중지·삭제하고, 자동으로 등록한 출력들의 원래 효과 설정도 복원한다. 상태는 앱의 장치 안내와 진단 기록, `C:\Program Files\PersonalTools\device-recovery-status.txt`에서 확인한다.

기존 설치는 관리자 PowerShell에서 `Scripts/update-windows-device-recovery.ps1 -SourceDirectory <새 빌드 패키지>`로 갱신한다. APO와 NPU 모델은 교체하지 않는다. 새 설치에는 자동 복구 도구가 포함된다.

## 설정 UI 디자인

설정 앱은 **C# / XAML 기반 WinUI 3**다. Windows App SDK 1.8과 .NET 8 런타임을 함께 배포한다. 기존 Win32/GDI 화면과 수동 스크롤·컨트롤 재배치 코드는 제거했다. C++ 오디오 엔진과 APO는 네이티브로 유지되며, UI 전용 C ABI DLL을 통해 설정과 상태만 전달한다. 오디오 콜백에 CLR을 로드하지 않는다.

- NavigationView에서 사운드 설정과 처리 상태·진단 페이지를 구분한다.
- Mica 배경, 테마 리소스, 실제 ToggleSwitch·ComboBox·InfoBar·ContentDialog를 사용한다.
- ScrollViewer의 합성 스크롤과 XAML 레이아웃을 사용한다. 스크롤마다 창 전체를 다시 그리거나 자식 HWND를 이동하지 않는다.
- 좁은 창에서는 좌우 EQ 카드가 세로로 배치되며, 탐색 메뉴는 접힌다.
- 장치 열거와 파일 읽기는 UI 스레드 밖에서 수행한다. 변경되지 않은 장치 목록·필터를 다시 바인딩하지 않아 선택과 스크롤 위치를 유지한다.
- 설치 전에는 필터 가져오기와 효과 스위치가 비활성이다. 필요한 파일과 미설치 이유를 화면에 표시한다.
- 설치된 장치 변경은 원래 설정 복원 후 다시 설치해야 한다. 실제 처리 상태와 설치 상태를 구분한다.
- 텍스트와 조작 컨트롤이 XAML 접근성 트리에 나타난다. 전체 스크린 리더·모든 배율 검증은 별도로 필요하다.

## 빌드

Visual Studio 2022의 **Desktop development with C++**, Windows 11 SDK (22000 이상), CMake 3.26 이상, **.NET 8 SDK**가 필요하다. WinUI 3는 NuGet 복원 후 dotnet publish로 빌드한다. WDK는 향후 서명된 드라이버 패키지 배포/HLK 검증 시 추가로 필요하며, 현재 로컬 APO 빌드는 WDK의 정적 base-APO 라이브러리에 의존하지 않는다.

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
dotnet publish Sources\Windows\WinUI\PersonalToolsWindows.csproj -c Release -r win-x64 --self-contained true -p:Platform=x64 -o build\windows-msvc\package\ui
```

결과물은 패키지 루트의 PersonalToolsAPO.dll·PersonalToolsSetup.exe·설치 스크립트와 ui 폴더의 WinUI 앱·브리지·런타임이다. **ui\PersonalToolsWindows.exe**로 실행한다. XAML 리소스인 PersonalToolsWindows.pri도 필요하므로 EXE만 따로 복사하지 않는다. 빌드와 테스트는 시스템 오디오에 APO를 등록하지 않는다. Windows CI는 MSVC 빌드·테스트와 WinUI 게시를 모두 수행한다.

이 PC에서 준비한 포터블 도구로 다시 빌드하려면 앱을 종료한 후 Scripts\build-windows-portable.ps1을 실행한다. 기본 출력은 build\winui-package다. 이 스크립트는 이미 내려받은 LLVM-MinGW, Microsoft Windows SDK NuGet 헤더, CMake, .NET SDK를 사용하며 시스템 설치나 다운로드를 수행하지 않는다. 경로는 매개변수로 바꿀 수 있다. 정식 MSVC 빌드는 위 기본 스크립트를 사용한다.

## 현재 PC 초기 설정

설정 창에서 다음을 선택한다.

- 출력: `스피커 (SMSL USB DAC)`
- 입력: `In 1-2 (MOTU M Series)` — Loopback/Loopback Mix가 아님
- 초기 왼쪽 EQ: `E:\Speaker\L.txt`
- 초기 오른쪽 EQ: `E:\Speaker\R.txt`

선택은 endpoint ID로 저장된다. 파일은 검증 후 `%ProgramData%\PersonalTools\L.txt`, `R.txt`로 복사되므로 이후 E: 드라이브가 없어도 동작한다. 최초 설치에서 위 파일이 없으면 초기화를 중단하며, 원래 오디오 효과 설정을 복원한다. 이후 설정 창에서 다른 REW 파일을 가져올 수 있다.

L: 66Hz/−12dB/Q7.40, 140Hz/−12dB/Q3.21.
R: 65Hz/−8.4dB/Q8.00, 142Hz/−12dB/Q3.36.

설정과 최근 64개 상태 변경 기록은 `%ProgramData%\PersonalTools`에 저장된다. 오디오 샘플은 기록하지 않는다. 설정 앱을 닫으면 트레이에 남고, 앱을 완전히 종료해도 이미 설치된 APO 효과는 유지된다.

## 설치 전 확인과 제한

기본 설치는 DLL/EXE의 유효한 Authenticode 서명을 요구한다. 이 PC에서 직접 빌드한 미서명 모듈을 사용하려면 UI의 **이 PC에서 미서명 오디오 모듈 사용**을 선택하거나 설치 명령에 `-LocalUnsigned`를 지정한다. 이 옵션은 `DisableProtectedAudioDG=1`로 PC 전체의 보호된 오디오 설정을 완화한다. 일부 보호된 콘텐츠 재생이 영향을 받을 수 있다. 기존 값과 존재 여부를 설치 백업에 기록하고 실패 또는 제거 시 복원한다. 손상되거나 신뢰되지 않는 서명은 이 옵션으로도 허용하지 않는다. Secure Boot와 커널 드라이버 서명 설정은 변경하지 않는다. 어느 설치 방식이든 실제 오디오 처리는 별도로 확인해야 한다.

다음은 오디오 설정을 바꾸지 않는 사전 점검이다. 이 PC에서 관찰한 ID이며 장치 재설치 후에는 설정 창에서 다시 선택한다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-windows.ps1 `
  -RenderId '{0.0.0.00000000}.{43406fc5-af54-479b-b33d-846022c5cbec}' `
  -CaptureId '{0.0.1.00000000}.{26202c49-c793-4233-8f53-acd9dc8efa05}' `
  -CheckOnly
```

서명 조건을 충족하거나 미서명 로컬 설치 옵션을 선택한 패키지에서는 설정 창의 **설치 및 적용**으로 관리자 설치를 실행한다. 설치 경로는 `%ProgramFiles%\PersonalTools`다. 복원용 원본 레지스트리 값은 일반 사용자가 수정할 수 없는 해당 폴더에 보관한다.

로컬 설치기는 기존 APO 체인이 없는 endpoint에만 연결한다. OEM 효과가 존재하면 덮어쓰지 않고 중단한다. 레지스트리 접근이 거부되면 소유권/ACL을 강제로 변경하지 않고 복원한다. 이 도구는 특정 PC용 COM/endpoint 등록 방식이며, Microsoft 서명·HLK 인증을 받은 범용 드라이버 INF 패키지가 아니다.

설치 후 재부팅하거나 Windows Audio 서비스를 다시 시작하고 재생·통화 앱을 다시 연다. 서비스 재시작은 진행 중인 재생과 녹음을 중단한다. **등록 성공은 처리 성공이 아니다.** 설정 창에서 실제 APO 콜백이 실행되는지 확인한다. 상태가 대기 중이면 오디오 정지, 효과 비활성화, ASIO/독점 모드 또는 APO 로딩 실패를 구분해 확인해야 한다. Windows 이벤트 뷰어의 CodeIntegrity/Operational 로그도 서명·로딩 실패 진단에 사용할 수 있다.

다른 장치로 변경하거나 바이너리를 업데이트하려면 기존 설치를 제거한 후 다시 설치한다. 제거는 원래 효과 등록을 복원하며, 필터 파일과 설정은 보존한다.

```powershell
# 관리자 PowerShell
powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall-windows.ps1
```

## 처리 동작

- 출력은 EFX APO에서 좌우 독립으로 처리한다. 지원 스테레오 float32 샘플레이트는 44.1/48/88.2/96/176.4/192kHz다. EQ에 별도 오디오 버퍼 지연은 없다.
- 마이크는 48kHz float32로 처리한다. 기본은 Windows 11 CAPX MFX이며, 실기기에서 MFX 미호출을 확인한 MOTU USB 인터페이스 `USB\VID_07FD&PID_000B&MI_00`은 설치기가 LFX 호환 방식을 자동 선택한다. UI와 명령줄 설치 모두 동일하게 적용하며 제품 표시 이름으로 판단하지 않는다. 입력 첫 채널만 사용하고 출력이 스테레오이면 처리된 모노를 양쪽에 전달한다. 모노 출력도 지원한다.
- MFX를 불러오지 않는 구형 캡처 드라이버를 위한 명시적 설치 옵션 `-LegacyCapture`도 있다. 이 경우 LFX 경로를 사용하고 실행 중인 PersonalTools 데스크톱 앱이 선택한 스피커의 WASAPI 루프백을 전달한다. 앱을 완전히 종료하면 참조가 끊기므로 반향 제거가 멈추고 마이크 원음을 통과시킨다. 기존의 다른 현대식 효과 등록을 자동으로 삭제하지 않으며 충돌 시 설치를 중단한다. 이 PC의 MOTU/SMSL 조합에서 실제 시스템 출력의 반향 감소를 확인했다. 측정 방법과 제한은 `windows-validation.md`의 최종 기록을 참고한다. 로그인 시 트레이 자동 실행은 현재 사용자로 `Scripts/register-windows-startup.ps1`을 실행해 등록한다. `--startup`은 설정 창을 띄우지 않고 오디오 참조 전달을 시작하며, 다시 앱을 열면 기존 창을 표시한다. 현재 사용자의 로그인 15초 후 일반 권한으로 실행하는 `PersonalTools` 예약 작업을 등록하며, 배터리 사용 중에도 실행한다. 작업 스케줄러에서 해당 작업을 사용 안 함으로 바꿀 수 있다. 기존 Run 자동 시작 항목은 중복 실행을 피하도록 제거한다.
- Windows가 제공하는 보조 스피커 참조와 마이크의 QPC 시각을 동일한 시간 단위로 변환한다. 참조 장치가 선택한 SMSL과 다르면 학습/제거를 수행하지 않는다. **Windows 기본 재생 장치 및 통화 앱의 재생 장치도 SMSL로 맞춘다.**
- 가변 콜백 길이를 256프레임 AEC 블록으로 연결한다. 참조 콜백의 도착 차이를 흡수하기 위해 **고정 1024프레임(21.33ms)** 마이크 지연을 추가한다. APO `GetLatency`가 이를 보고한다. AEC를 끄더라도 스트림 시간축을 유지하기 위해 동일한 지연으로 원음을 전달한다.
- 참조 누락은 지연된 마이크 원음으로 통과한다. 큐 초과·타임스탬프 불연속·참조 장치 변경은 학습 상태를 초기화한다. 입력 과부하 및 동시 발화 보호는 기존 AEC를 유지한다.
- 설정 변경은 버전이 있는 고정 크기 공유 상태로 전달된다. 필터 해석과 계수 생성은 UI에서 수행하고, 콜백에는 사전 할당된 연산 상태만 사용한다. 처리 중 파일 접근/동적 할당/잠금 대기를 하지 않는다.
- 지원 대상은 Windows 공유 모드의 기본·통신 처리 경로다. RAW, ASIO, WASAPI 독점 모드, 시스템 오디오 효과를 끈 앱/장치는 우회할 수 있다. 물리 마이크의 펌웨어나 ASIO 입력을 수정하는 방식이 아니다.
- 장치 재연결은 Windows 장치 알림 및 오디오 엔진의 APO 재생성으로 처리한다. 앱이 MOTU 샘플레이트를 강제로 전환하거나 Windows Audio 서비스를 임의로 재시작하지 않는다. 기존 통화 앱이 끊긴 스트림을 재개하지 않는 경우 앱에서 입력 장치를 다시 열어야 한다.

## 검증 구분

MOTU에서 녹음 중에도 마이크 처리 상태가 비활성이고 MFX 콜백이 발생하지 않는 기존 설치는 관리자 PowerShell에서 `Scripts\repair-windows-capture.ps1`로 LFX 호환 경로로 전환할 수 있다. `-CheckOnly`는 변경 없이 사전 점검한다. 다른 효과가 있으면 중단하며, 기존 EQ/필터/설정을 유지하고 제거용 백업에 새로 변경하는 값만 추가한다. Windows Audio를 재시작하므로 재생/녹음 스트림을 다시 열어야 한다. PersonalTools 앱이 실행 중이어야 스피커 참조가 전달된다. 전환 성공과 실제 반향 감소는 별도로 검증한다.

자동 검증은 기존 코어/AEC 회귀, 타임스탬프·가변 패킷·큐 초과, Windows Unicode 경로 재가져오기, APO 형식 협상·바이패스·COM 수명, DLL 로딩, 설치 실패 복원을 포함한다. WinUI 브리지 테스트는 미초기화 상태, 공유 설정 토글, 유효/무효 필터 가져오기, 실패 시 기존 필터 보존, 잘못된 장치 ID 거부를 검증한다. APO·브리지 테스트는 임시 설정 폴더에서 실행하고, 복원 테스트는 임시 HKCU 키만 사용한다.

실제 방의 반향 감소, SMSL·MOTU 드라이버의 APO 수용 여부, audiodg의 서명 검사 통과, 실제 통화 앱과의 호환성은 **선택한 설치 방식으로 설치한 후 별도 검증해야 한다**. 합성 테스트 결과를 실제 장치 성능으로 간주하지 않는다.

개발 참고: [Windows 11 AEC 인터페이스](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-11-apis-for-audio-processing-objects), [APO 구현·설치](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects).
