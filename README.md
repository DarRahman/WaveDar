# WaveDar

<p align="center">
  <img src="assets/app_logo.png" width="140" alt="WaveDar Logo">
  <br>
  <b>Lightweight native audio player for Windows</b>
</p>

WaveDar is a minimalist, standalone audio player for Windows written in C99. It streams local audio directly to Windows WASAPI with zero network traffic, no background telemetry, and no runtime dependencies.

## Features

- **Low Memory Footprint**: Uses around 1.8 MB to 2.5 MB of private memory during active playback.
- **Native Audio Streaming**: Direct audio output via miniaudio and Windows WASAPI.
- **Format Support**: Plays MP3, WAV, FLAC, and OGG Vorbis (with sample-accurate duration).
- **Auto-Skip Resilience**: Corrupted or unreadable tracks are automatically skipped without halting the playlist.
- **Native Unicode Console**: Renders Japanese (Kanji/Kana), Korean, Chinese, and multi-language titles cleanly without question marks.
- **Persistent Session**: Automatically saves and restores your last folder, track index, volume, and playback mode.
- **Clean Metadata**: Strips common downloader tags (y2mate, Spotify, NCS, 320kbps) to display clean artist and song titles.
- **100% Offline**: Zero network sockets, zero HTTP requests, and completely standalone.

## Keyboard Controls

| Key | Action |
| :--- | :--- |
| `Space` | Play / Pause |
| `Left` / `Right` | Seek backward / forward 5 seconds |
| `[` / `]` | Seek backward / forward 15 seconds |
| `n` / `p` | Next / Previous track |
| `+` / `-` | Volume up / down (also Up / Down arrows) |
| `r` | Toggle single track repeat |
| `s` | Toggle shuffle mode (Fisher-Yates deck) |
| `o` | Open folder (paste path or drag-and-drop) |
| `q` | Save session and quit |

Global media keys (Play/Pause, Next Track, Prev Track, Stop) are also supported in the background.

## Building from Source

Requirements:
- Windows 10 or 11
- Zig compiler (`zig cc`) or any C99 compiler (MSVC / Clang / GCC)

Run the build script:
```cmd
build.bat
```

This compiles a single standalone binary: `wavedar.exe` (no installers or external DLLs required).

## License

MIT License.
