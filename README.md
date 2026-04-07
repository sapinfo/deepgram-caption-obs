# Deepgram Captions for OBS

[English](README.en.md) | **한국어**

OBS Studio 플러그인으로 Deepgram Nova-3 API를 사용하여 실시간 음성 자막을 표시합니다.

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-donate-yellow)](https://buymeacoffee.com/inseokko)

## 주요 기능

- **실시간 음성-텍스트 변환**: Deepgram Nova-3 모델 사용
- **50+ 언어 지원**: 한국어, 영어, 일본어, 중국어 등
- **다중 언어 자동 감지**: Multilingual 모드 지원
- **실시간 중간 결과**: 말하는 중에도 즉시 자막 표시
- **Smart Format**: 자동 구두점, 숫자 변환
- **핫키 지원**: OBS 핫키로 자막 시작/중지
- **크로스 플랫폼**: macOS, Windows, Linux 지원

## 설치

### 사전 빌드 릴리즈 (권장)

1. [Releases](https://github.com/sapinfo/deepgram-caption-obs/releases) 페이지에서 최신 릴리즈 다운로드
2. 플랫폼별 설치:
   - **macOS**: `.pkg` 파일 실행 또는 `.plugin` 폴더를 `~/Library/Application Support/obs-studio/plugins/`에 복사
   - **Windows**: `.exe` 인스톨러 실행 또는 `.zip`을 OBS 플러그인 폴더에 압축 해제
   - **Linux**: `.deb` 패키지 설치

### 직접 빌드

```bash
# macOS
cmake --preset macos
cmake --build --preset macos

# OBS에 설치
cp -R build_macos/RelWithDebInfo/deepgram-caption-obs.plugin \
  ~/Library/Application\ Support/obs-studio/plugins/
```

## 사용법

1. **Deepgram API 키 발급**: [Deepgram Console](https://console.deepgram.com/)에서 API 키 생성
2. **OBS에서 소스 추가**: Sources → + → Deepgram Captions
3. **설정**:
   - API Key 입력
   - Audio Source 선택 (마이크 등)
   - 언어 선택
   - 모델 선택 (Nova-3 권장)
4. **Test Connection** 버튼으로 연결 확인
5. **Start Caption** 버튼으로 자막 시작

## 설정 옵션

| 옵션 | 설명 | 기본값 |
|------|------|--------|
| Model | Deepgram 모델 선택 | Nova-3 |
| Language | 인식 언어 | Korean |
| Smart Format | 자동 서식 적용 | ON |
| Punctuation | 구두점 추가 | ON |
| Interim Results | 중간 결과 표시 | ON |
| Endpointing | 발화 종료 감지 (ms) | 300ms |
| Font | 자막 폰트 | Apple SD Gothic Neo / Malgun Gothic |
| Font Size | 자막 크기 | 48 |

## 지원 언어

한국어, 영어(US/UK), 일본어, 중국어(간체/번체), 스페인어, 프랑스어, 독일어, 포르투갈어, 이탈리아어, 네덜란드어, 러시아어, 힌디어 등 50+ 언어

## 기술 스택

- **C++17** / CMake 3.28+
- **OBS SDK** 31.1.1
- **IXWebSocket** v11.4.5 (WebSocket 클라이언트)
- **nlohmann/json** v3.11.3 (JSON 파싱)
- **OpenSSL 3.x** (TLS)

## 라이선스

GPL-2.0 - 자세한 내용은 [LICENSE](LICENSE) 파일을 참조하세요.
