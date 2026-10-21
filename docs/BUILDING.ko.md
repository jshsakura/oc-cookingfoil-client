# 빌드와 미리보기

[전체 안내](README.ko.md) · [English](BUILDING.md) · [작업 규칙 (영문)](CONTRIBUTING-GUIDE.md)

CookingFoil은 C++17과 SDL2를 사용합니다. 호스트와 본체 빌드가 UI와 핵심 로직을 공유하고,
본체 전용 서비스에는 호스트용 대체 구현이 있습니다.

## 소스 받기

```sh
git clone --recursive https://github.com/jshsakura/oc-cookingfoil-client
cd oc-cookingfoil-client
```

이미 받은 저장소에서는 `git submodule update --init --recursive`로 서브모듈을 준비합니다.

## 호스트 의존성

Ubuntu에서는 압축 파일 지원을 위한 minizip까지 설치합니다.

```sh
sudo apt-get install build-essential pkg-config libsdl2-dev libsdl2-ttf-dev \
  libsdl2-image-dev libjson-c-dev libcurl4-openssl-dev libssl-dev zlib1g-dev \
  libjpeg-dev libminizip-dev fonts-noto-cjk ripgrep
make test host
```

SQLite와 zstd 디코더는 저장소에 포함된 소스를 빌드합니다. minizip이 없으면 경고와 함께
ZIP 지원을 제외하므로 전체 호스트 기능을 확인하려면 설치합니다.

## 브라우저 미리보기

```sh
./build-host/cookingfoil-host --fixture host/fixtures --lang ko --serve 38520
```

`http://localhost:38520/`을 엽니다. 실제 UI를 거치 모드 1920×1080으로 그리며,
키보드, 버튼, 마우스와 드래그 입력을 받습니다. `--lang en`, `ja`, `zh`로 다른 언어를 확인합니다.

앱 자체의 여러 서버 설정 흐름을 확인할 때는 다음과 같이 실행합니다.

```sh
./build-host/cookingfoil-host --app-config path/to/config.json \
  --state out/preview/cookingfoil.db --serve 38520
```

`--config path/to/config.json`은 첫 번째 켜진 서버를 미리보기 세션에 연결하는 별도 모드입니다.
앱의 설정 읽기와 여러 서버를 확인할 때는 `--app-config`를 사용합니다.

미리보기는 모든 인터페이스에서 인증 없이 접속을 받습니다. 신뢰할 수 있는 네트워크에서 실행하고
확인이 끝나면 종료합니다. 설치, 세이브 접근, 삭제와 파일 정리는 실기기에서 확인합니다.

## 스크린샷

```sh
./build-host/cookingfoil-host --fixture host/fixtures --lang en --shot out/screens-en
```

주요 화면과 예제 상태를 PNG로 저장합니다. 일반 테스트 데이터에는 예제 그림이 들어 있습니다.
문서와 웹사이트에 공개하는 스크린샷은 문서 언어와 관계없이 영문 UI를 사용합니다.
아이콘이 없는 상태를 보여 줄 때는 예제 메타데이터의 그림 URL을 비워 실제 UI의 첫 글자 타일을
촬영합니다. 예제 그림을 아이콘이 없을 때의 대체 표시로 사용하지 않습니다.

확인한 화면만 `docs/screenshots`에 넣습니다. 생성 데이터와 개인 서버 설정은 Git에 넣지 않습니다.
[CI](../.github/workflows/ci.yml)도 호스트 스크린샷을 결과물로 올립니다.

## 본체 빌드

Docker가 있고 `DEVKITPRO`가 설정되지 않았다면 다음을 실행합니다.

```sh
make
```

`devkitpro/devkita64`에서 고정된 CyberFoil에 `third_party/patches`를 적용하고
`cookingfoil.nro`를 만듭니다. 첫 빌드는 `scripts/build-tls.sh`로 고정된 curl과 mbedTLS 소스를
내려받아 빌드하므로 네트워크 연결이 필요합니다.

직접 설치한 [devkitPro](https://devkitpro.org/wiki/Getting_Started) 환경에서는
Switch portlibs를 준비하고 `DEVKITPRO`를 설정한 채 같은 명령을 실행합니다.

```sh
dkp-pacman -S switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-curl switch-mbedtls \
  switch-json-c switch-freetype switch-harfbuzz switch-libwebp switch-libpng \
  switch-libjpeg-turbo switch-zlib switch-bzip2 switch-minizip
make
```

버전은 `makefiles/switch.mk`의 `APP_VERSION`에서 읽으며 `make version`으로 확인합니다.
릴리스 태그는 이 버전과 일치해야 합니다. 변경은 PR로 반영하고, 빌드 결과물은 Git에 넣지 않습니다.
배포 절차는 [작업 규칙의 Shipping 절 (영문)](CONTRIBUTING-GUIDE.md#shipping)을 따릅니다.

## PR 전 확인

`make test`를 실행하고, UI 변경은 본체에 넣기 전에 브라우저에서 확인합니다.
동작을 바꾸면 그 동작을 확인하는 테스트를 추가하며, 파일 배치와 삭제는 특히 확인합니다.
[디자인 (영문)](DESIGN.md), [문구 (영문)](WRITING.md), [작업 규칙 (영문)](CONTRIBUTING-GUIDE.md)도 읽습니다.
