# CLAUDE.md - Deepgram Captions OBS Plugin

## Project Overview
OBS Studio plugin providing real-time speech-to-text captions using Deepgram Nova-3 API.
Single-file C++ plugin (`src/plugin-main.cpp`) with WebSocket streaming.

## Tech Stack
- **Language**: C++17
- **Build**: CMake 3.28+ / Xcode (macOS) / VS 2022 (Windows) / Ninja (Linux)
- **OBS SDK**: 31.1.1 (via buildspec.json auto-download)
- **Dependencies**: IXWebSocket v11.4.5 (WebSocket), nlohmann/json v3.11.3, OpenSSL 3.x
- **Platforms**: macOS (arm64 + x86_64), Windows (x64), Ubuntu (x86_64)

## Build Commands
```bash
# macOS local build
cmake --preset macos
cmake --build --preset macos

# Install to OBS
cp -R build_macos/RelWithDebInfo/deepgram-caption-obs.plugin \
  ~/Library/Application\ Support/obs-studio/plugins/

# Clean rebuild (after adding/removing source files)
rm -rf build_macos && cmake --preset macos

# Release (tag triggers CI build for all platforms)
git tag x.x.x && git push origin x.x.x
```

## CI/CD
- **Trigger**: Tag push only (`on.push.tags`). Main branch push does NOT trigger Actions.
- **Builds**: macOS arm64 + x86_64 (cross-compile), Windows x64, Ubuntu x86_64
- **Release**: Automatic draft release on valid semver tag (e.g., `0.1.0`)
- **Codesigning**: Not configured. Builds are unsigned.

## Key Architecture Decisions
- Single source file (`plugin-main.cpp`) - all plugin logic in one file
- Uses OBS built-in text source (`text_ft2_source_v2` / `text_gdiplus`) for rendering
- Audio: OBS float32 48kHz -> int16 16kHz downsampled -> WebSocket binary to Deepgram
- Deepgram auth via HTTP Authorization header (not JSON body like Soniox)
- Deepgram config via URL query parameters (not initial JSON message)
- Test Connection: sends `CloseStream` + `stop()` immediately after successful Open to avoid leaving idle connections
- KeepAlive thread sends `{"type":"KeepAlive"}` every 5 seconds to prevent timeout
- Button text toggle uses `obs_property_set_description()` in callback (not `get_properties` re-invocation). See `docs/OBS-Plugin-Button-Toggle-Fix.md`
- x86_64 macOS CI: Intel Homebrew OpenSSL at `/usr/local/opt/openssl@3`
- `CMakeLists.txt` guards `OPENSSL_ROOT_DIR` with `NOT DEFINED CACHE{}` to prevent preset override
- Text style properties (font/color/outline/shadow/width/wrap) use `obs_properties_add_font` + platform-specific text source settings (`text_gdiplus` on Windows, `text_ft2_source_v2` on macOS/Linux) — same pattern as Soniox plugin

## Deepgram API Notes
- WebSocket URL: `wss://api.deepgram.com/v1/listen` with query parameters
- Auth: `Authorization: Token <key>` HTTP header
- Response types: `Metadata`, `Results`, `UtteranceEnd`, `SpeechStarted`, `Error`
- `is_final` = finalized transcript, `speech_final` = speaker finished utterance
- Must send KeepAlive every 5s or connection drops after 10s silence
- Send `{"type":"CloseStream"}` for graceful disconnect
- No built-in translation (STT only, unlike Soniox)

## Important Conventions
- Version in `buildspec.json` (single source of truth)
- Commit messages: imperative mood, explain "why" not "what"
- GitHub repo: `sapinfo/deepgram-caption-obs`
- README style: language switch links at top, Buy Me A Coffee section at bottom (for-the-badge style)
