# 시작하기

[전체 안내](README.ko.md) · [기능 안내](FEATURES.ko.md) · [빌드하기](BUILDING.ko.md) · [English](GETTING-STARTED.md)

## 클라이언트 설치

홈브루를 실행할 수 있는 본체와 사용 권한이 있는 CookingFoil 서버가 필요합니다.
서버 설치는 [서버의 한국어 안내](https://github.com/jshsakura/oc-cookingfoil/blob/main/README.ko.md#빠른-시작)를 따릅니다.

1. 서버 웹페이지에 로그인해 `cookingfoil.nro`를 받습니다. 운영자가 먼저 `data/client/`에 빌드를 넣거나 관리 페이지에서 올려야 합니다. [클라이언트 연결 안내](https://github.com/jshsakura/oc-cookingfoil/blob/main/README.ko.md#클라이언트-연결)를 참고합니다.
2. SD 카드의 `sdmc:/switch/cookingfoil/cookingfoil.nro`에 복사합니다.
3. 홈브루 메뉴에서 실행합니다. 세이브 접근에는 애플리케이션 모드가 필요하므로, 게임을 실행할 때 R을 누른 채 홈브루 메뉴를 엽니다.
4. **설정 → 서버 추가**에서 주소와 필요한 인증 정보를 넣고 저장합니다.

처음 설치하면 서버 목록은 비어 있습니다. 다른 앱의 서버 설정을 가져오지 않습니다.

## 기기 승인

승인이 필요한 서버에 연결하면 본체에 QR 코드가 표시됩니다. 휴대폰으로 스캔하고 해당 서버에
로그인한 뒤, 승인 권한이 있는 계정으로 기기를 승인합니다. 승인이 끝나면 본체가 연결되고
접근 키를 `cookingfoil.db`에 보관합니다.

승인은 서버마다 따로 관리합니다. 한 서버의 승인을 취소해도 다른 서버의 승인이 바뀌지 않습니다.
기기 인증 정보는 해당 본체에 묶이므로 데이터베이스를 복사해 다른 본체로 승인을 옮길 수 없습니다.

## PC에서 설정하기

NRO 옆에 `config.json`을 넣어도 됩니다. 서버의 사용자 관리 화면에서 설정 파일을 받거나,
로그인한 브라우저에서 `/api/client-config`로 내려받을 수 있습니다. 서버가 만든 파일에는
비밀번호와 비밀 키가 포함되지 않습니다.

```json
{
  "servers": [
    {
      "title": "내 서버",
      "url": "https://files.example.com",
      "enabled": true
    }
  ],
  "language": "ko",
  "installTarget": "sd"
}
```

| 키 | 뜻 |
|---|---|
| `servers[].title` | 앱에 보이는 이름. 생략하면 호스트 이름을 사용합니다 |
| `servers[].url` | HTTP 또는 HTTPS 서버 주소 |
| `servers[].enabled` | 연결 여부. 기본값은 `true`입니다 |
| `servers[].username`, `servers[].password` | 필요한 경우의 HTTP Basic 인증 정보 |
| `servers[].cfClientId`, `servers[].cfClientSecret` | 필요한 경우의 Cloudflare Access 서비스 토큰 |
| `servers[].enrollToken` | 서버가 제공한 등록 토큰. 사용 후 파일에서 제거합니다 |
| `language` | `ko`, `en`, `ja`, `zh` 중 하나 |
| `installTarget` | `sd`는 SD 카드, `nand`는 본체 저장 공간 |

[설정 예시](config.example.json)에는 꺼 둔 예비 서버와 선택 인증 항목도 있습니다. 예제 주소를
자신의 서버 주소로 바꿉니다. URL에 계정 정보, 쿼리나 프래그먼트를 넣지 않습니다.
프로토콜 없이 호스트 이름만 넣으면 HTTPS로 연결합니다.

PC에서 파일을 고친 뒤에는 설정 화면에 표시되는 다시 읽기 동작을 사용합니다. 앱에서 서버를
고치면 `config.json`에 저장합니다. 언어와 같은 앱 설정은 데이터베이스에 저장되며, 설정 파일의
초깃값보다 먼저 적용될 수 있습니다.

## 여러 서버 사용

설정에서 서버를 추가하거나 `servers` 배열에 항목을 넣습니다. 최대 32개까지 지원합니다.
켜 둔 서버의 목록을 함께 보여 주며, 항목마다 원래 서버를 기억합니다. 같은 항목이 여러 서버에
있으면 받을 서버를 고를 수 있습니다. 서버가 여러 개일 때는 이름과 색으로 구분합니다.

홈브루, 패치, 레트로와 연동 기능은 각 서버가 제공하는 기능에 따라 표시됩니다.
선택 메뉴가 보이지 않으면 서버에 해당 콘텐츠가 있는지 확인합니다.

## SD 카드의 파일

| 위치 | 용도 |
|---|---|
| `switch/cookingfoil/cookingfoil.nro` | 앱 실행 파일 |
| `switch/cookingfoil/config.json` | 서버 설정 |
| `switch/cookingfoil/cookingfoil.db` | 앱 설정, 기기 인증, 다운로드 대기열과 기록 |
| `switch/cookingfoil/log.txt` | 문제 확인에 사용하는 앱 로그 |
| `switch/cookingfoil/patches/` | 받은 패치와 배치 기록 |
| `switch/oc-save-keeper/` | 로컬 세이브 백업과 save-keeper 공용 설정 |

이전 버전의 `state.json`은 기기 정보를 확인하고 저장할 수 있을 때 데이터베이스로 옮깁니다.
앱을 업데이트할 때는 NRO만 교체하고 데이터베이스와 설정 파일을 보관합니다.

## 연결 확인

- 같은 네트워크의 다른 기기에서 서버 주소가 열리는지 확인합니다.
- 승인 대기 중이면 휴대폰에서 해당 서버의 기기 승인을 마칩니다.
- 서버가 HTTP Basic이나 Cloudflare Access 인증을 요구하면 해당 항목을 입력합니다.
- 애플릿 모드에서 세이브 접근이 막히면 앱의 안내에 따라 애플리케이션 모드로 실행합니다.
- 선택 기능이 보이지 않으면 서버의 콘텐츠와 설정을 확인합니다.
