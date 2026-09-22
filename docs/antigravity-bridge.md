# Antigravity CLI 브릿지

Personal Tools의 Windows GUI에서 **Antigravity 브릿지** 탭을 선택한다.
오디오 APO 설치 없이도 브릿지를 사용할 수 있으며, 사운드 기능과 독립적으로 실행된다.

## 시작

1. [공식 Antigravity CLI 안내](https://antigravity.google/docs/cli/overview)에 따라 CLI를 설치한다.
2. 탭의 CLI 경로를 확인한다. 기본값은 `%LOCALAPPDATA%\agy\bin\agy.exe`다.
3. **CLI 열기 · 로그인**에서 Google 계정 로그인을 완료한다.
4. **CLI · 모델 확인**으로 계정에 제공되는 실제 모델 목록을 가져온다.
5. 기본 모델, 포트, 제한 시간을 정하고 **브릿지 시작**을 누른다.
6. **연결 주소 · 키 복사**로 클라이언트에 설정한다. **테스트 전송**으로 실제 응답을 확인할 수 있다.

기본 Base URL은 `http://127.0.0.1:8877/v1`이다. API 키는 처음 실행할 때 생성되어
`%LOCALAPPDATA%\PersonalTools\Antigravity\api-key.txt`에 저장되며 재실행해도 유지된다.
화면에서 확인하거나 복사할 수 있다. 이 파일은 로컬 접속 비밀 키이므로 공유하지 않는다.
CLI 경로·포트·기본 모델·제한 시간은 시작할 때
`%LOCALAPPDATA%\PersonalTools\Antigravity\settings.json`에 저장된다.
앱 실행 시 저장된 설정으로 브릿지가 자동 시작된다. 설정이 없으면 기본 설정을 사용한다.
트레이로 시작할 때도 자동 시작하며, CLI 경로 오류나 포트 충돌은 브릿지 탭에 표시한다.
수동으로 중지한 뒤에는 앱을 다시 실행하거나 **브릿지 시작**을 누를 때까지 중지 상태를 유지한다.
창을 닫으면 트레이에서 유지되고 **앱 종료**하면 중지된다.

```python
from openai import OpenAI

client = OpenAI(
    base_url="http://127.0.0.1:8877/v1",
    api_key="GUI에서 복사한 API 키",
)
print([m.id for m in client.models.list().data])
response = client.chat.completions.create(
    model="antigravity",  # CLI 기본 모델 또는 모델 목록의 정확한 ID
    messages=[{"role": "user", "content": "안녕하세요"}],
)
print(response.choices[0].message.content)

for chunk in client.chat.completions.create(
    model="antigravity",
    messages=[{"role": "user", "content": "짧은 인사말을 써 주세요"}],
    stream=True,
    stream_options={"include_usage": True},
):
    if chunk.choices:
        print(chunk.choices[0].delta.content or "", end="", flush=True)
```

## 지원 범위

| 항목 | 동작 |
| --- | --- |
| `GET /v1/models` | `agy models`의 실제 모델 ID + `antigravity` 기본 모델 별칭 |
| `POST /v1/chat/completions` | 일반 JSON 응답 또는 실시간 SSE |
| `GET /health` | HTTP 서버 상태. CLI 로그인 여부는 모델 확인·테스트 전송으로 확인 |
| `messages` | system/developer/user/assistant, 문자열 또는 text 블록 |
| `model` | 생략 시 GUI 기본 모델. 지정하면 그대로 CLI에 전달 |
| `stream`, `stream_options.include_usage` | 역할·텍스트 delta, stop, 선택적 usage, `[DONE]` |
| `n` | 1만 지원 |
| 토큰 사용량 | CLI가 보고한 값. thinking은 output에 포함되므로 중복 합산하지 않음 |

Responses API, legacy completions, 이미지·오디오, 외부 도구 호출, 구조화 출력,
max_tokens 등 생성 옵션은 현재 지원하지 않는다. PastePaw 호환을 위해 `temperature`는
0~2 또는 null을 허용하지만 CLI에 적용하지 않고 `X-Unsupported-Params: temperature` 응답 헤더로 알린다.
그 밖의 지원하지 않는 입력은
조용히 무시하지 않고 OpenAI 오류 객체와 HTTP 400으로 알린다.
메시지 역할과 대화 이력은 하나의 텍스트 프롬프트로 직렬화된다. 이는 네이티브
OpenAI 역할 분리와 동일한 보장을 제공하지 않는다. 각 요청은 별도 CLI 대화로 실행된다.

## 실행과 오류 처리

WinUI 프로세스 안에서 Kestrel이 루프백 IPv4 주소만 수신한다. Python/Node 서버 설치나
관리자 권한, HTTP.sys URL 예약은 필요 없다. 모든 경로에 Bearer 인증을 적용하고 CORS를 허용하지 않는다.
키는 Google OAuth 토큰이 아니라 이 앱의 로컬 접속 키다. Google 인증은 CLI가 직접 처리한다.

`agy.exe --output-format stream-json --sandbox --disable-slash-commands --print-timeout … -p …`
를 shell 없이 `ProcessStartInfo.ArgumentList`로 실행한다. 실제 `agent_response.text_delta`만
클라이언트에 전달하고, 성공 result와 프로세스 종료를 확인한 뒤 완료 이벤트를 보낸다.
권한 자동 승인 플래그는 사용하지 않는다. 별도 작업 폴더
`%LOCALAPPDATA%\PersonalTools\Antigravity\workspace`에서 실행하며 기존 CLI 권한 설정은 유지된다.
CLI는 에이전트이므로 모델에 도구를 사용하지 말라고 요청하는 것 자체가 보안 격리는 아니다.
브릿지 키는 신뢰하는 로컬 클라이언트에만 제공한다.

요청은 동시에 하나만 처리하고 나머지는 429로 반환한다. 기본 제한 시간은 120초다.
취소·연결 종료·시간 초과·앱 종료 시 실행 중인 CLI 프로세스 트리를 종료한다.
일반 응답의 CLI 오류는 502, 시간 초과는 504다. SSE 시작 후 오류는 error 이벤트와
`[DONE]`으로 전달하며 성공 stop 이벤트를 보내지 않는다.
HTTP 본문 한도는 128 KiB, CLI 출력 한도는 4 Mi 문자다. Windows 명령행 한도를
보수적으로 계산하여 긴 대화를 400으로 거부하므로 클라이언트에서 이력을 줄여야 할 수 있다.

앱은 프롬프트·응답·키를 로그에 기록하지 않는다. CLI 자체의 대화 기록과 데이터 처리는
Antigravity 설정을 따른다. 모델·계정 오류는 **CLI 열기 · 로그인**에서 확인한다.

## 공개 구현 참고

- [starhunt/star-cliproxy](https://github.com/starhunt/star-cliproxy), 특히
  [agy-provider.ts](https://github.com/starhunt/star-cliproxy/blob/main/packages/server/src/providers/agy-provider.ts):
  실제 CLI 프로세스, 플래그 순서, stream-json 이벤트 및 result 처리 방식을 검토했다.
- [benteckxyz/agy-openai-proxy](https://github.com/benteckxyz/agy-openai-proxy):
  macOS PTY에 의존하는 구현이므로 Windows 앱에 직접 포함하지 않았다.
- [공식 CLI](https://github.com/google-antigravity/antigravity-cli)와 설치된 `agy --help`로
  실행 옵션을 확인했다.

외부 프록시 코드를 복사하거나 의존성으로 추가하지 않고 해당 프로토콜을 C#으로 구현했다.
2026-09-22 설치된 Windows CLI의 모델 조회, 실제 JSON 응답과 SSE를 검증했다.

## 개발 검증

```powershell
dotnet run --project Tests/AntigravityBridge/AntigravityBridgeTests.csproj -c Release
# 실제 로그인된 CLI로 모델 조회 및 두 번의 생성 요청 실행
dotnet run --project Tests/AntigravityBridge/AntigravityBridgeTests.csproj -c Release -- --live
dotnet publish Sources/Windows/WinUI/PersonalToolsWindows.csproj -c Release -r win-x64 --self-contained true -p:Platform=x64 -o build/personal-tools-package/ui
```

첫 테스트는 실제 자식 프로세스로 실행되는 가짜 CLI를 사용해 인증, 모델 목록, UTF-8,
SSE/usage, 잘못된 입력, CLI 실패, 크기 한도, 429, timeout, 중지·재시작을 확인한다.
기존 오디오 설치 경로, 실행 파일명, 단일 인스턴스 식별자는 업데이트 호환성을 위해 유지한다.
