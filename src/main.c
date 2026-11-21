#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <wchar.h>
#include <time.h>
#include <conio.h>

#include "../include/stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "../include/miniaudio.h"
#include "metadata.h"
#include "net_cover.h"

#define DISCORD_CLIENT_ID "1572952110713012377"
#define MAX_TRACKS 256
#define MAX_PATH_LEN 512

typedef struct {
    wchar_t full_path[MAX_PATH_LEN];
    wchar_t filename[MAX_TITLE_LEN];
    TrackMetadata meta;
} TrackInfo;

static TrackInfo g_playlist[MAX_TRACKS];
static int g_playlist_count = 0;
static int g_current_index = 0;
static int g_shuffle_order[MAX_TRACKS];
static int g_shuffle_index = 0;

static ma_decoder g_decoder;
static ma_device g_device;
static bool g_device_init = false;
static bool g_decoder_init = false;
static CRITICAL_SECTION g_audio_cs;

static volatile bool g_is_playing = false;
static volatile bool g_is_paused = false;
static volatile bool g_repeat_single = false;
static volatile bool g_is_shuffle = false;
static volatile bool g_track_finished = false;
static volatile bool g_loop_restarted = false;
static volatile float g_volume = 0.15f;

static bool g_discord_enabled = true;
static HANDLE g_discord_pipe = INVALID_HANDLE_VALUE;
static uint64_t g_discord_nonce = 1;
static time_t g_last_discord_reconnect = 0;

static char g_current_cover_url[MAX_URL_LEN] = "app_logo";
static char g_current_album_name[MAX_ALBUM_LEN] = "WaveDar";
static CRITICAL_SECTION g_cover_cs;

static void clean_quotes_w(wchar_t* str) {
    if (!str || !*str) return;
    while (*str == L' ' || *str == L'\t' || *str == L'\r' || *str == L'\n') {
        memmove(str, str + 1, (wcslen(str) + 1) * sizeof(wchar_t));
    }
    size_t len = wcslen(str);
    while (len > 0 && (str[len - 1] == L' ' || str[len - 1] == L'\t' || str[len - 1] == L'\r' || str[len - 1] == L'\n')) {
        str[--len] = L'\0';
    }
    if (len >= 2 && ((str[0] == L'\"' && str[len - 1] == L'\"') || (str[0] == L'\'' && str[len - 1] == L'\''))) {
        memmove(str, str + 1, (len - 1) * sizeof(wchar_t));
        str[len - 2] = L'\0';
        len -= 2;
    }
    while (len >= 3 && str[0] == L'\\' && iswalpha(str[1]) && str[2] == L':') {
        memmove(str, str + 1, len * sizeof(wchar_t));
        len--;
    }
    while (len > 0 && (str[len - 1] == L'\\' || str[len - 1] == L'/')) {
        str[--len] = L'\0';
    }
}

static void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;
    EnterCriticalSection(&g_audio_cs);
    if (!g_decoder_init || g_is_paused) {
        memset(pOutput, 0, frameCount * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
        LeaveCriticalSection(&g_audio_cs);
        return;
    }

    ma_uint64 framesRead = 0;
    ma_result result = ma_decoder_read_pcm_frames(&g_decoder, pOutput, frameCount, &framesRead);

    if (result != MA_SUCCESS || framesRead < frameCount) {
        if (g_repeat_single) {
            ma_decoder_seek_to_pcm_frame(&g_decoder, 0);
            if (framesRead < frameCount) {
                ma_uint64 extraFrames = 0;
                void* pExtraOutput = (char*)pOutput + (framesRead * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
                ma_decoder_read_pcm_frames(&g_decoder, pExtraOutput, frameCount - framesRead, &extraFrames);
                framesRead += extraFrames;
            }
            g_loop_restarted = true;
        } else {
            if (framesRead < frameCount) {
                void* pEmpty = (char*)pOutput + (framesRead * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
                memset(pEmpty, 0, (frameCount - framesRead) * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
            }
            g_track_finished = true;
        }
    }
    LeaveCriticalSection(&g_audio_cs);
}

static int get_cjk_string_width(const wchar_t* str) {
    if (!str) return 0;
    int w = 0;
    while (*str) {
        wchar_t c = *str++;
        if (c >= 0x1100 && (
            (c <= 0x115F) || c == 0x2329 || c == 0x232A ||
            (c >= 0x2E80 && c <= 0xA4CF && c != 0x303F) ||
            (c >= 0xAC00 && c <= 0xD7A3) ||
            (c >= 0xF900 && c <= 0xFAFF) ||
            (c >= 0xFE10 && c <= 0xFE19) ||
            (c >= 0xFE30 && c <= 0xFE6F) ||
            (c >= 0xFF00 && c <= 0xFF60) ||
            (c >= 0xFFE0 && c <= 0xFFE6))) {
            w += 2;
        } else {
            w += 1;
        }
    }
    return w;
}

static void get_current_track_time(int* p_cur, int* p_tot) {
    int cur_sec = 0, tot_sec = 0;
    if (g_decoder_init) {
        ma_uint64 cursorFrames = 0, totalFrames = 0;
        EnterCriticalSection(&g_audio_cs);
        ma_decoder_get_cursor_in_pcm_frames(&g_decoder, &cursorFrames);
        ma_decoder_get_length_in_pcm_frames(&g_decoder, &totalFrames);
        if (g_decoder.outputSampleRate > 0) {
            cur_sec = (int)(cursorFrames / g_decoder.outputSampleRate);
            tot_sec = (int)(totalFrames / g_decoder.outputSampleRate);
        }
        LeaveCriticalSection(&g_audio_cs);
    }
    if (p_cur) *p_cur = cur_sec;
    if (p_tot) *p_tot = tot_sec;
}

static HANDLE discord_connect(void) {
    if (!g_discord_enabled) return INVALID_HANDLE_VALUE;

    for (int i = 0; i < 10; i++) {
        wchar_t pipe_name[64];
        _snwprintf(pipe_name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            char json[256];
            snprintf(json, sizeof(json), "{\"v\":1,\"client_id\":\"%s\"}", DISCORD_CLIENT_ID);
            uint32_t op = 0; // HANDSHAKE
            uint32_t len = (uint32_t)strlen(json);
            DWORD written = 0;
            WriteFile(h, &op, 4, &written, NULL);
            WriteFile(h, &len, 4, &written, NULL);
            WriteFile(h, json, len, &written, NULL);

            uint32_t resp_op = 0, resp_len = 0;
            DWORD read_bytes = 0;
            if (ReadFile(h, &resp_op, 4, &read_bytes, NULL) &&
                ReadFile(h, &resp_len, 4, &read_bytes, NULL)) {
                char* resp_buf = (char*)malloc(resp_len + 1);
                if (resp_buf) {
                    ReadFile(h, resp_buf, resp_len, &read_bytes, NULL);
                    free(resp_buf);
                }
            }
            return h;
        }
    }
    return INVALID_HANDLE_VALUE;
}

static void discord_escape_json(const char* src, char* dst, size_t dst_len) {
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 2 < dst_len; si++) {
        unsigned char c = (unsigned char)src[si];
        if (c == '\"') {
            dst[di++] = '\\'; dst[di++] = '\"';
        } else if (c == '\\') {
            dst[di++] = '\\'; dst[di++] = '\\';
        } else if (c == '\n') {
            dst[di++] = '\\'; dst[di++] = 'n';
        } else if (c == '\r') {
            dst[di++] = '\\'; dst[di++] = 'r';
        } else if (c == '\t') {
            dst[di++] = '\\'; dst[di++] = 't';
        } else if (c >= 0x20) {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
}

static void discord_update_presence(const TrackInfo* track, int cur_sec, int tot_sec, bool is_paused) {
    if (!g_discord_enabled) return;

    time_t now = time(NULL);
    if (g_discord_pipe == INVALID_HANDLE_VALUE) {
        if (now - g_last_discord_reconnect < 5) return;
        g_last_discord_reconnect = now;
        g_discord_pipe = discord_connect();
        if (g_discord_pipe == INVALID_HANDLE_VALUE) return;
    }

    char u_song[MAX_TITLE_LEN * 3];
    char u_artist[MAX_ARTIST_LEN * 3];
    WideCharToMultiByte(CP_UTF8, 0, track->meta.title, -1, u_song, sizeof(u_song), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, track->meta.artist, -1, u_artist, sizeof(u_artist), NULL, NULL);

    char esc_song[MAX_TITLE_LEN * 3];
    char esc_artist[MAX_ARTIST_LEN * 3];
    discord_escape_json(u_song, esc_song, sizeof(esc_song));
    discord_escape_json(u_artist, esc_artist, sizeof(esc_artist));

    char cover_url[MAX_URL_LEN];
    char album_name[MAX_ALBUM_LEN];
    EnterCriticalSection(&g_cover_cs);
    strncpy(cover_url, g_current_cover_url, sizeof(cover_url) - 1);
    cover_url[sizeof(cover_url) - 1] = '\0';
    strncpy(album_name, g_current_album_name, sizeof(album_name) - 1);
    album_name[sizeof(album_name) - 1] = '\0';
    LeaveCriticalSection(&g_cover_cs);

    char esc_album[MAX_ALBUM_LEN * 3];
    discord_escape_json(album_name, esc_album, sizeof(esc_album));

    char payload[4096];
    DWORD pid = GetCurrentProcessId();

    long long start_ts = (long long)(now - cur_sec);
    long long end_ts = (long long)(start_ts + tot_sec);

    if (is_paused) {
        snprintf(payload, sizeof(payload),
            "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{"
            "\"type\":2,"
            "\"status_display_type\":2,"
            "\"details\":\"%s\","
            "\"state\":\"%s\","
            "\"assets\":{"
            "\"large_image\":\"%s\","
            "\"large_text\":\"%s\","
            "\"small_image\":\"app_logo\","
            "\"small_text\":\"WaveDar (Paused)\""
            "}"
            "}},\"nonce\":\"%llu\"}",
            (unsigned long)pid, esc_song, esc_artist, cover_url, esc_album, (unsigned long long)g_discord_nonce++);
    } else {
        snprintf(payload, sizeof(payload),
            "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{"
            "\"type\":2,"
            "\"status_display_type\":2,"
            "\"details\":\"%s\","
            "\"state\":\"%s\","
            "\"timestamps\":{\"start\":%lld,\"end\":%lld},"
            "\"assets\":{"
            "\"large_image\":\"%s\","
            "\"large_text\":\"%s\","
            "\"small_image\":\"app_logo\","
            "\"small_text\":\"WaveDar\""
            "}"
            "}},\"nonce\":\"%llu\"}",
            (unsigned long)pid, esc_song, esc_artist, start_ts, end_ts, cover_url, esc_album, (unsigned long long)g_discord_nonce++);
    }

    uint32_t op = 1; // FRAME
    uint32_t len = (uint32_t)strlen(payload);
    DWORD written = 0;
    if (!WriteFile(g_discord_pipe, &op, 4, &written, NULL) ||
        !WriteFile(g_discord_pipe, &len, 4, &written, NULL) ||
        !WriteFile(g_discord_pipe, payload, len, &written, NULL)) {
        CloseHandle(g_discord_pipe);
        g_discord_pipe = INVALID_HANDLE_VALUE;
    }
}

static void discord_clear_presence(void) {
    if (g_discord_pipe != INVALID_HANDLE_VALUE) {
        char payload[256];
        DWORD pid = GetCurrentProcessId();
        snprintf(payload, sizeof(payload),
            "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":null},\"nonce\":\"%llu\"}",
            (unsigned long)pid, (unsigned long long)g_discord_nonce++);
        uint32_t op = 1;
        uint32_t len = (uint32_t)strlen(payload);
        DWORD written = 0;
        WriteFile(g_discord_pipe, &op, 4, &written, NULL);
        WriteFile(g_discord_pipe, &len, 4, &written, NULL);
        WriteFile(g_discord_pipe, payload, len, &written, NULL);
        CloseHandle(g_discord_pipe);
        g_discord_pipe = INVALID_HANDLE_VALUE;
    }
}

static void on_cover_resolved(const wchar_t* title, const char* cover_url, const char* album_name) {
    if (!title || !cover_url) return;

    if (_wcsicmp(g_playlist[g_current_index].meta.title, title) == 0) {
        EnterCriticalSection(&g_cover_cs);
        strncpy(g_current_cover_url, cover_url, sizeof(g_current_cover_url) - 1);
        g_current_cover_url[sizeof(g_current_cover_url) - 1] = '\0';
        strncpy(g_current_album_name, album_name, sizeof(g_current_album_name) - 1);
        g_current_album_name[sizeof(g_current_album_name) - 1] = '\0';
        LeaveCriticalSection(&g_cover_cs);

        int cur = 0, tot = 0;
        get_current_track_time(&cur, &tot);
        discord_update_presence(&g_playlist[g_current_index], cur, tot, g_is_paused);
    }
}

static bool play_track(int index) {
    if (index < 0 || index >= g_playlist_count) return false;

    int attempts = 0;
    int cur_idx = index;

    while (attempts < g_playlist_count) {
        if (g_device_init) {
            ma_device_stop(&g_device);
        }

        EnterCriticalSection(&g_audio_cs);
        if (g_decoder_init) {
            ma_decoder_uninit(&g_decoder);
            g_decoder_init = false;
        }

        ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 2, 44100);
        ma_result result = ma_decoder_init_file_w(g_playlist[cur_idx].full_path, &decoderConfig, &g_decoder);
        if (result == MA_SUCCESS) {
            g_decoder_init = true;
            LeaveCriticalSection(&g_audio_cs);
            break;
        }
        LeaveCriticalSection(&g_audio_cs);

        attempts++;
        cur_idx = (cur_idx + 1) % g_playlist_count;
    }

    if (attempts >= g_playlist_count) {
        return false;
    }

    index = cur_idx;

    if (!g_device_init) {
        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
        deviceConfig.playback.format   = g_decoder.outputFormat;
        deviceConfig.playback.channels = g_decoder.outputChannels;
        deviceConfig.sampleRate        = g_decoder.outputSampleRate;
        deviceConfig.dataCallback      = data_callback;
        deviceConfig.pUserData         = NULL;

        if (ma_device_init(NULL, &deviceConfig, &g_device) != MA_SUCCESS) {
            ma_decoder_uninit(&g_decoder);
            g_decoder_init = false;
            return false;
        }
        g_device_init = true;
    }

    ma_device_set_master_volume(&g_device, g_volume);
    ma_device_start(&g_device);

    g_current_index = index;
    g_is_playing = true;
    g_is_paused = false;
    g_track_finished = false;

    EnterCriticalSection(&g_cover_cs);
    strncpy(g_current_cover_url, "app_logo", sizeof(g_current_cover_url) - 1);
    g_current_cover_url[sizeof(g_current_cover_url) - 1] = '\0';
    strncpy(g_current_album_name, "WaveDar", sizeof(g_current_album_name) - 1);
    g_current_album_name[sizeof(g_current_album_name) - 1] = '\0';
    LeaveCriticalSection(&g_cover_cs);

    int cur = 0, tot = 0;
    get_current_track_time(&cur, &tot);
    discord_update_presence(&g_playlist[index], cur, tot, false);

    if (g_discord_enabled) {
        net_cover_resolve_async(g_playlist[index].meta.title, g_playlist[index].meta.artist, on_cover_resolved);
    }

    return true;
}

static bool load_directory(const wchar_t* dir_path) {
    g_playlist_count = 0;

    wchar_t clean_dir[MAX_PATH_LEN];
    wcsncpy(clean_dir, dir_path, MAX_PATH_LEN - 1);
    clean_dir[MAX_PATH_LEN - 1] = L'\0';
    clean_quotes_w(clean_dir);
    size_t dlen = wcslen(clean_dir);
    while (dlen > 0 && (clean_dir[dlen - 1] == L'\\' || clean_dir[dlen - 1] == L'/')) {
        clean_dir[--dlen] = L'\0';
    }

    wchar_t search_pattern[MAX_PATH_LEN];
    _snwprintf(search_pattern, MAX_PATH_LEN, L"%s\\*.*", clean_dir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(search_pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
            if (ext && (_wcsicmp(ext, L".mp3") == 0 || _wcsicmp(ext, L".wav") == 0 ||
                        _wcsicmp(ext, L".flac") == 0 || _wcsicmp(ext, L".ogg") == 0 ||
                        _wcsicmp(ext, L".m4a") == 0)) {
                if (g_playlist_count < MAX_TRACKS) {
                    _snwprintf(g_playlist[g_playlist_count].full_path, MAX_PATH_LEN, L"%s\\%s", clean_dir, fd.cFileName);
                    wcsncpy(g_playlist[g_playlist_count].filename, fd.cFileName, MAX_TITLE_LEN - 1);
                    g_playlist[g_playlist_count].filename[MAX_TITLE_LEN - 1] = L'\0';

                    extract_track_metadata(g_playlist[g_playlist_count].full_path,
                                           g_playlist[g_playlist_count].filename,
                                           &g_playlist[g_playlist_count].meta);

                    g_shuffle_order[g_playlist_count] = g_playlist_count;
                    g_playlist_count++;
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    return (g_playlist_count > 0);
}

static void apply_shuffle(bool shuffle_on) {
    g_is_shuffle = shuffle_on;
    if (g_playlist_count <= 0) return;

    if (!g_is_shuffle) {
        g_current_index = g_shuffle_order[g_shuffle_index];
    } else {
        srand((unsigned int)time(NULL));
        for (int i = g_playlist_count - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int temp = g_shuffle_order[i];
            g_shuffle_order[i] = g_shuffle_order[j];
            g_shuffle_order[j] = temp;
        }
        for (int i = 0; i < g_playlist_count; i++) {
            if (g_shuffle_order[i] == g_current_index) {
                g_shuffle_index = i;
                break;
            }
        }
    }
}

static void next_track(void) {
    if (g_playlist_count == 0) return;
    int next_idx = 0;
    if (g_is_shuffle) {
        g_shuffle_index = (g_shuffle_index + 1) % g_playlist_count;
        next_idx = g_shuffle_order[g_shuffle_index];
    } else {
        next_idx = (g_current_index + 1) % g_playlist_count;
    }
    play_track(next_idx);
}

static void prev_track(void) {
    if (g_playlist_count == 0) return;
    int prev_idx = 0;
    if (g_is_shuffle) {
        g_shuffle_index = (g_shuffle_index - 1 + g_playlist_count) % g_playlist_count;
        prev_idx = g_shuffle_order[g_shuffle_index];
    } else {
        prev_idx = (g_current_index - 1 + g_playlist_count) % g_playlist_count;
    }
    play_track(prev_idx);
}

static void draw_shortcuts_box(void) {
    wprintf(L"---------------------------------------------------------------------------------\n");
    wprintf(L" [Space] Play/Pause | [Left/Right] Seek 5s | [n/p] Next/Prev | [q] Quit          \n");
    wprintf(L" [ + / - ] Volume   | [ r ] Repeat         | [ s ] Shuffle   | [o] Open Folder   \n");
    wprintf(L"---------------------------------------------------------------------------------\n");
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(L"WaveDar");

    int w_argc = 0;
    LPWSTR* w_argv = CommandLineToArgvW(GetCommandLineW(), &w_argc);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    DWORD mode = 0;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    DWORD in_mode = 0;
    GetConsoleMode(hIn, &in_mode);
    SetConsoleMode(hIn, (in_mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);

    CONSOLE_CURSOR_INFO cursor_info = { 1, FALSE };
    SetConsoleCursorInfo(hOut, &cursor_info);

    wchar_t initial_dir[MAX_PATH_LEN] = {0};
    if (w_argv) {
        for (int i = 1; i < w_argc; i++) {
            if (_wcsicmp(w_argv[i], L"--no-discord") == 0) {
                g_discord_enabled = false;
            } else if (initial_dir[0] == L'\0') {
                wcsncpy(initial_dir, w_argv[i], MAX_PATH_LEN - 1);
                clean_quotes_w(initial_dir);
            }
        }
    }

    if (initial_dir[0] == L'\0') {
        wprintf(L"\n");
        wprintf(L"=================================================================================\n");
        wprintf(L"                            Welcome to WaveDar Player                            \n");
        wprintf(L"=================================================================================\n\n");
        wprintf(L"Please drop a music folder here, or paste folder path and press Enter:\n");
        wprintf(L"(Or just press Enter to use default Music library)\n> ");

        cursor_info.bVisible = TRUE;
        SetConsoleCursorInfo(hOut, &cursor_info);

        if (fgetws(initial_dir, MAX_PATH_LEN, stdin)) {
            size_t len = wcslen(initial_dir);
            while (len > 0 && (initial_dir[len - 1] == L'\n' || initial_dir[len - 1] == L'\r' || initial_dir[len - 1] == L' ')) {
                initial_dir[--len] = L'\0';
            }
            clean_quotes_w(initial_dir);
        }

        if (initial_dir[0] == L'\0') {
            const wchar_t* profile = _wgetenv(L"USERPROFILE");
            if (profile) {
                _snwprintf(initial_dir, MAX_PATH_LEN, L"%s\\Music", profile);
            }
        }

        cursor_info.bVisible = FALSE;
        SetConsoleCursorInfo(hOut, &cursor_info);
    }

    if (!load_directory(initial_dir)) {
        wprintf(L"\n[Error] No audio files (.mp3, .wav, .flac) found in target path.\n");
        wprintf(L"Press any key to exit...\n");
        _getch();
        if (w_argv) LocalFree(w_argv);
        return 1;
    }

    InitializeCriticalSection(&g_audio_cs);
    InitializeCriticalSection(&g_cover_cs);
    net_cover_init();

    RegisterHotKey(NULL, 1, 0, VK_MEDIA_PLAY_PAUSE);
    RegisterHotKey(NULL, 2, 0, VK_MEDIA_NEXT_TRACK);
    RegisterHotKey(NULL, 3, 0, VK_MEDIA_PREV_TRACK);
    RegisterHotKey(NULL, 4, 0, VK_MEDIA_STOP);

    wprintf(L"\x1b[2J\x1b[H");
    draw_shortcuts_box();

    play_track(0);

    COORD status_coord = { 0, 5 };
    bool is_running = true;

    while (is_running) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY) {
                if (msg.wParam == 1) {
                    if (g_device_init) {
                        if (g_is_paused) { ma_device_start(&g_device); g_is_paused = false; }
                        else { ma_device_stop(&g_device); g_is_paused = true; }
                        int cur = 0, tot = 0; get_current_track_time(&cur, &tot);
                        discord_update_presence(&g_playlist[g_current_index], cur, tot, g_is_paused);
                    }
                } else if (msg.wParam == 2) {
                    next_track();
                } else if (msg.wParam == 3) {
                    prev_track();
                } else if (msg.wParam == 4) {
                    is_running = false;
                }
            }
        }

        while (_kbhit()) {
            int ch = _getch();
            if (ch == 0 || ch == 0xE0) {
                int arrow = _getch();
                if (arrow == 75) {
                    if (g_decoder_init) {
                        EnterCriticalSection(&g_audio_cs);
                        ma_uint64 cur_pcm = 0;
                        ma_decoder_get_cursor_in_pcm_frames(&g_decoder, &cur_pcm);
                        ma_uint64 seek_frames = (ma_uint64)g_decoder.outputSampleRate * 5;
                        ma_decoder_seek_to_pcm_frame(&g_decoder, cur_pcm > seek_frames ? cur_pcm - seek_frames : 0);
                        LeaveCriticalSection(&g_audio_cs);

                        int cur = 0, tot = 0; get_current_track_time(&cur, &tot);
                        discord_update_presence(&g_playlist[g_current_index], cur, tot, g_is_paused);
                    }
                } else if (arrow == 77) {
                    if (g_decoder_init) {
                        EnterCriticalSection(&g_audio_cs);
                        ma_uint64 cur_pcm = 0, len_pcm = 0;
                        ma_decoder_get_cursor_in_pcm_frames(&g_decoder, &cur_pcm);
                        ma_decoder_get_length_in_pcm_frames(&g_decoder, &len_pcm);
                        ma_uint64 seek_frames = (ma_uint64)g_decoder.outputSampleRate * 5;
                        ma_decoder_seek_to_pcm_frame(&g_decoder, cur_pcm + seek_frames < len_pcm ? cur_pcm + seek_frames : len_pcm);
                        LeaveCriticalSection(&g_audio_cs);

                        int cur = 0, tot = 0; get_current_track_time(&cur, &tot);
                        discord_update_presence(&g_playlist[g_current_index], cur, tot, g_is_paused);
                    }
                } else if (arrow == 72) {
                    g_volume = (g_volume + 0.05f > 1.0f) ? 1.0f : g_volume + 0.05f;
                    if (g_device_init) ma_device_set_master_volume(&g_device, g_volume);
                } else if (arrow == 80) {
                    g_volume = (g_volume - 0.05f < 0.0f) ? 0.0f : g_volume - 0.05f;
                    if (g_device_init) ma_device_set_master_volume(&g_device, g_volume);
                }
            } else if (ch == ' ') {
                if (g_device_init) {
                    if (g_is_paused) { ma_device_start(&g_device); g_is_paused = false; }
                    else { ma_device_stop(&g_device); g_is_paused = true; }
                    int cur = 0, tot = 0; get_current_track_time(&cur, &tot);
                    discord_update_presence(&g_playlist[g_current_index], cur, tot, g_is_paused);
                }
            } else if (ch == 'n' || ch == 'N') {
                next_track();
            } else if (ch == 'p' || ch == 'P') {
                prev_track();
            } else if (ch == '+' || ch == '=') {
                g_volume = (g_volume + 0.05f > 1.0f) ? 1.0f : g_volume + 0.05f;
                if (g_device_init) ma_device_set_master_volume(&g_device, g_volume);
            } else if (ch == '-' || ch == '_') {
                g_volume = (g_volume - 0.05f < 0.0f) ? 0.0f : g_volume - 0.05f;
                if (g_device_init) ma_device_set_master_volume(&g_device, g_volume);
            } else if (ch == 'r' || ch == 'R') {
                g_repeat_single = !g_repeat_single;
            } else if (ch == 's' || ch == 'S') {
                apply_shuffle(!g_is_shuffle);
            } else if (ch == 'o' || ch == 'O') {
                COORD prompt_coord = { 0, 7 };
                SetConsoleCursorPosition(hOut, prompt_coord);
                wprintf(L"\x1b[2K[Open Folder] Drop folder or paste path (Esc to cancel):\n> \x1b[2K");

                cursor_info.bVisible = TRUE;
                SetConsoleCursorInfo(hOut, &cursor_info);

                wchar_t new_dir[MAX_PATH_LEN] = {0};
                size_t nlen = 0;
                bool cancelled = false;

                while (true) {
                    wint_t wc = _getwch();
                    if (wc == 27) {
                        cancelled = true;
                        break;
                    } else if (wc == L'\r' || wc == L'\n') {
                        break;
                    } else if (wc == L'\b') {
                        if (nlen > 0) {
                            new_dir[--nlen] = L'\0';
                            wprintf(L"\b \b");
                        }
                    } else if (wc >= 32 && nlen + 1 < MAX_PATH_LEN) {
                        new_dir[nlen++] = (wchar_t)wc;
                        new_dir[nlen] = L'\0';
                        putwchar(wc);
                    }
                }

                cursor_info.bVisible = FALSE;
                SetConsoleCursorInfo(hOut, &cursor_info);

                clean_quotes_w(new_dir);

                SetConsoleCursorPosition(hOut, (COORD){0, 7});
                wprintf(L"\x1b[2K\n\x1b[2K");

                if (!cancelled && nlen > 0) {
                    if (load_directory(new_dir)) {
                        play_track(0);
                    } else {
                        SetConsoleCursorPosition(hOut, (COORD){0, 7});
                        wprintf(L"[Notice] No audio files found in directory.\x1b[2K");
                        Sleep(1500);
                        SetConsoleCursorPosition(hOut, (COORD){0, 7});
                        wprintf(L"\x1b[2K");
                    }
                }
            } else if (ch == '[' || ch == '{') {
                if (g_decoder_init) {
                    EnterCriticalSection(&g_audio_cs);
                    ma_uint64 cur_pcm = 0;
                    ma_decoder_get_cursor_in_pcm_frames(&g_decoder, &cur_pcm);
                    ma_uint64 seek_frames = (ma_uint64)g_decoder.outputSampleRate * 15;
                    ma_decoder_seek_to_pcm_frame(&g_decoder, cur_pcm > seek_frames ? cur_pcm - seek_frames : 0);
                    LeaveCriticalSection(&g_audio_cs);

                    int cur = 0, tot = 0; get_current_track_time(&cur, &tot);
                    discord_update_presence(&g_playlist[g_current_index], cur, tot, g_is_paused);
                }
            } else if (ch == ']' || ch == '}') {
                if (g_decoder_init) {
                    EnterCriticalSection(&g_audio_cs);
                    ma_uint64 cur_pcm = 0, len_pcm = 0;
                    ma_decoder_get_cursor_in_pcm_frames(&g_decoder, &cur_pcm);
                    ma_decoder_get_length_in_pcm_frames(&g_decoder, &len_pcm);
                    ma_uint64 seek_frames = (ma_uint64)g_decoder.outputSampleRate * 15;
                    ma_decoder_seek_to_pcm_frame(&g_decoder, cur_pcm + seek_frames < len_pcm ? cur_pcm + seek_frames : len_pcm);
                    LeaveCriticalSection(&g_audio_cs);

                    int cur = 0, tot = 0; get_current_track_time(&cur, &tot);
                    discord_update_presence(&g_playlist[g_current_index], cur, tot, g_is_paused);
                }
            } else if (ch == 'q' || ch == 'Q') {
                is_running = false;
            }
        }

        if (g_track_finished) {
            g_track_finished = false;
            next_track();
        }

        int cur_sec = 0, tot_sec = 0;
        get_current_track_time(&cur_sec, &tot_sec);

        if (g_loop_restarted) {
            g_loop_restarted = false;
            discord_update_presence(&g_playlist[g_current_index], cur_sec, tot_sec, false);
        }

        static int discord_tick = 0;
        discord_tick++;
        if (discord_tick % 150 == 0) {
            discord_update_presence(&g_playlist[g_current_index], cur_sec, tot_sec, g_is_paused);
        }

        SetConsoleCursorPosition(hOut, status_coord);

        wchar_t display_title[MAX_TITLE_LEN * 2];
        _snwprintf(display_title, sizeof(display_title) / sizeof(wchar_t), L"%s - %s",
                   g_playlist[g_current_index].meta.title, g_playlist[g_current_index].meta.artist);

        int max_title_chars = 48;
        wchar_t clipped_title[64];
        wcsncpy(clipped_title, display_title, max_title_chars);
        clipped_title[max_title_chars] = L'\0';
        if (wcslen(display_title) > (size_t)max_title_chars) {
            clipped_title[max_title_chars - 3] = L'.';
            clipped_title[max_title_chars - 2] = L'.';
            clipped_title[max_title_chars - 1] = L'.';
        }

        int visual_width = get_cjk_string_width(clipped_title);
        int padding_needed = 52 - visual_width;
        if (padding_needed < 0) padding_needed = 0;

        wchar_t line_buf[512];
        int line_len = _snwprintf(line_buf, sizeof(line_buf) / sizeof(wchar_t),
            L"[%s]  [%02d:%02d/%02d:%02d]  [%2.0f%%]  [%s]  #%d/%d: %s%*s\r",
            g_is_paused ? L"PAUSE" : L"PLAY ",
            cur_sec / 60, cur_sec % 60,
            tot_sec / 60, tot_sec % 60,
            g_volume * 100.0f,
            g_repeat_single ? L"Rep:1" : (g_is_shuffle ? L"Shuf " : L"Loop "),
            g_current_index + 1, g_playlist_count,
            clipped_title,
            padding_needed, L"");

        DWORD written = 0;
        if (!WriteConsoleW(hOut, line_buf, (DWORD)line_len, &written, NULL)) {
            char u8_line[1024];
            int u8_len = WideCharToMultiByte(CP_UTF8, 0, line_buf, line_len, u8_line, sizeof(u8_line) - 1, NULL, NULL);
            if (u8_len > 0) {
                WriteFile(hOut, u8_line, (DWORD)u8_len, &written, NULL);
            }
        }

        Sleep(50);
    }

    discord_clear_presence();
    net_cover_cleanup();
    DeleteCriticalSection(&g_cover_cs);
    DeleteCriticalSection(&g_audio_cs);

    if (g_device_init) {
        ma_device_stop(&g_device);
        ma_device_uninit(&g_device);
    }
    if (g_decoder_init) {
        ma_decoder_uninit(&g_decoder);
    }

    UnregisterHotKey(NULL, 1);
    UnregisterHotKey(NULL, 2);
    UnregisterHotKey(NULL, 3);
    UnregisterHotKey(NULL, 4);

    if (w_argv) LocalFree(w_argv);

    cursor_info.bVisible = TRUE;
    SetConsoleCursorInfo(hOut, &cursor_info);
    wprintf(L"\n\nExiting WaveDar cleanly...\n");
    return 0;
}
