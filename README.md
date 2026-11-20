# WaveDar

WaveDar is an ultra-lightweight, zero-bloat native audio player for Windows written in pure C99. Designed to deliver responsive, instant playback while preserving system resources.

## Highlights

- **Ultra Low Memory Footprint**: ~1.0 MB - 2.0 MB private memory during active playback.
- **Native Windows Audio**: Direct streaming using miniaudio and Windows WASAPI.
- **Dynamic Metadata & Cover Art**: Extracts tags and resolves artwork via native WinHTTP without heavy web browsers or frameworks.
- **Flicker-Free Terminal UI**: Clean keyboard controls with visual CJK character width support.
- **Discord Rich Presence**: Real-time track progress and presence updates.
- **Standalone Portability**: Single executable, zero runtime dependencies.

## Keyboard Controls

| Key | Action |
| :--- | :--- |
| `Space` | Play / Pause |
| `Left` / `Right` | Seek backward / forward 5 seconds |
| `[` / `]` | Seek backward / forward 15 seconds |
| `n` / `p` | Next / Previous track |
| `+` / `-` | Volume up / down |
| `r` | Toggle repeat mode |
| `s` | Toggle shuffle mode |
| `o` | Open folder / change music directory |
| `q` | Quit application |

## Building from Source

Requirements:
- Windows 10 / 11
- Zig compiler or Clang / GCC

Run the build script:
```cmd
build.bat
```

## License

This project is licensed under the MIT License.
