# Deepgram Captions for OBS

**English** | [한국어](README.md)

An OBS Studio plugin that provides real-time speech-to-text captions using the Deepgram Nova-3 API.

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-donate-yellow)](https://buymeacoffee.com/inseokko)

## Features

- **Real-time Speech-to-Text**: Powered by Deepgram Nova-3 model
- **50+ Languages**: Korean, English, Japanese, Chinese, and more
- **Multilingual Auto-Detection**: Automatic language switching
- **Live Interim Results**: See captions as you speak
- **Smart Format**: Automatic punctuation and number formatting
- **Hotkey Support**: Start/stop captions via OBS hotkeys
- **Cross-Platform**: macOS, Windows, Linux

## Installation

### Pre-built Releases (Recommended)

1. Download the latest release from [Releases](https://github.com/sapinfo/deepgram-caption-obs/releases)
2. Install for your platform:
   - **macOS**: Run the `.pkg` installer or copy `.plugin` folder to `~/Library/Application Support/obs-studio/plugins/`
   - **Windows**: Run the `.exe` installer or extract `.zip` to OBS plugins folder
   - **Linux**: Install the `.deb` package

### Build from Source

```bash
# macOS
cmake --preset macos
cmake --build --preset macos

# Install to OBS
cp -R build_macos/RelWithDebInfo/deepgram-caption-obs.plugin \
  ~/Library/Application\ Support/obs-studio/plugins/
```

## Usage

1. **Get a Deepgram API Key**: Sign up at [Deepgram Console](https://console.deepgram.com/)
2. **Add Source in OBS**: Sources → + → Deepgram Captions
3. **Configure**:
   - Enter your API Key
   - Select Audio Source (e.g., microphone)
   - Choose language
   - Select model (Nova-3 recommended)
4. Click **Test Connection** to verify
5. Click **Start Caption** to begin

## Settings

| Option | Description | Default |
|--------|-------------|---------|
| Model | Deepgram model | Nova-3 |
| Language | Recognition language | Korean |
| Smart Format | Auto formatting | ON |
| Punctuation | Add punctuation | ON |
| Interim Results | Show partial results | ON |
| Endpointing | End-of-speech detection (ms) | 300ms |
| Font | Caption font | Apple SD Gothic Neo / Malgun Gothic |
| Font Size | Caption size | 48 |

## Supported Languages

Korean, English (US/UK), Japanese, Chinese (Simplified/Traditional), Spanish, French, German, Portuguese, Italian, Dutch, Russian, Hindi, and 50+ more languages.

## Tech Stack

- **C++17** / CMake 3.28+
- **OBS SDK** 31.1.1
- **IXWebSocket** v11.4.5 (WebSocket client)
- **nlohmann/json** v3.11.3 (JSON parsing)
- **OpenSSL 3.x** (TLS)

## License

GPL-2.0 - See [LICENSE](LICENSE) for details.
