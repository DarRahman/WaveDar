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

static int g_current_track_total_sec = 0;
static wchar_t g_current_loaded_dir[MAX_PATH_LEN] = {0};

static void get_state_file_path(wchar_t* out_path, size_t max_len) {
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    if (appdata && appdata[0] != L'\0') {
        wchar_t dir[MAX_PATH_LEN];
        _snwprintf(dir, MAX_PATH_LEN, L"%s\\WaveDar", appdata);
        CreateDirectoryW(dir, NULL);
        _snwprintf(out_path, max_len, L"%s\\state.ini", dir);
    } else {
        _snwprintf(out_path, max_len, L"wavedar_state.ini");
    }
}

static void save_state(const wchar_t* dir, int track_idx, float vol, bool repeat, bool shuffle) {
    if (!dir || dir[0] == L'\0') return;
    wchar_t state_path[MAX_PATH_LEN];
    get_state_file_path(state_path, MAX_PATH_LEN);

    FILE* f = _wfopen(state_path, L"w, ccs=UTF-8");
    if (f) {
        fwprintf(f, L"[WaveDar]\n");
        fwprintf(f, L"dir=%s\n", dir);
        fwprintf(f, L"track=%d\n", track_idx);
        fwprintf(f, L"volume=%.2f\n", vol);
        fwprintf(f, L"repeat=%d\n", repeat ? 1 : 0);
        fwprintf(f, L"shuffle=%d\n", shuffle ? 1 : 0);
        fclose(f);
    }
}

static BOOL WINAPI console_ctrl_handler(DWORD ctrlType) {
    (void)ctrlType;
    save_state(g_current_loaded_dir, g_current_index, g_volume, g_repeat_single, g_is_shuffle);
    return FALSE;
}

static bool load_state(wchar_t* out_dir, int* out_track, float* out_vol, bool* out_repeat, bool* out_shuffle) {
    wchar_t state_path[MAX_PATH_LEN];
    get_state_file_path(state_path, MAX_PATH_LEN);

    FILE* f = _wfopen(state_path, L"r, ccs=UTF-8");
    if (!f) return false;

    wchar_t line[MAX_PATH_LEN];
    bool has_dir = false;
    while (fgetws(line, MAX_PATH_LEN, f)) {
        size_t len = wcslen(line);
        while (len > 0 && (line[len - 1] == L'\n' || line[len - 1] == L'\r')) {
            line[--len] = L'\0';
        }
        if (wcsncmp(line, L"dir=", 4) == 0) {
            wcsncpy(out_dir, line + 4, MAX_PATH_LEN - 1);
            out_dir[MAX_PATH_LEN - 1] = L'\0';
            if (out_dir[0] != L'\0') has_dir = true;
        } else if (wcsncmp(line, L"track=", 6) == 0) {
            if (out_track) *out_track = _wtoi(line + 6);
        } else if (wcsncmp(line, L"volume=", 7) == 0) {
            if (out_vol) *out_vol = (float)_wtof(line + 7);
        } else if (wcsncmp(line, L"repeat=", 7) == 0) {
            if (out_repeat) *out_repeat = (_wtoi(line + 7) != 0);
        } else if (wcsncmp(line, L"shuffle=", 8) == 0) {
            if (out_shuffle) *out_shuffle = (_wtoi(line + 8) != 0);
        }
    }
    fclose(f);
    return has_dir;
}

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
    for (size_t i = 0; str[i] != L'\0'; i++) {
        wchar_t ch = str[i];
        if ((ch >= 0x1100 && ch <= 0x11FF) ||
            (ch >= 0x2E80 && ch <= 0x9FFF) ||
            (ch >= 0xAC00 && ch <= 0xD7AF) ||
            (ch >= 0xF900 && ch <= 0xFAFF) ||
            (ch >= 0xFE30 && ch <= 0xFE4F) ||
            (ch >= 0xFF00 && ch <= 0xFF60) ||
            (ch >= 0xFFE0 && ch <= 0xFFE6)) {
            w += 2;
        } else {
            w += 1;
        }
    }
    return w;
}

static void get_current_track_time(int* p_cur, int* p_tot) {
    int cur_sec = 0, tot_sec = g_current_track_total_sec;
    if (g_decoder_init) {
        ma_uint64 cursorFrames = 0;
        EnterCriticalSection(&g_audio_cs);
        ma_decoder_get_cursor_in_pcm_frames(&g_decoder, &cursorFrames);
        if (g_decoder.outputSampleRate > 0) {
            cur_sec = (int)(cursorFrames / g_decoder.outputSampleRate);
        }
        LeaveCriticalSection(&g_audio_cs);
    }
    if (p_cur) *p_cur = cur_sec;
    if (p_tot) *p_tot = tot_sec;
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

    ma_uint64 totalFrames = 0;
    ma_decoder_get_length_in_pcm_frames(&g_decoder, &totalFrames);
    if (g_decoder.outputSampleRate > 0 && totalFrames > 0) {
        g_current_track_total_sec = (int)(totalFrames / g_decoder.outputSampleRate);
    } else {
        g_current_track_total_sec = 0;
        const wchar_t* ext = wcsrchr(g_playlist[index].full_path, L'.');
        if (ext && _wcsicmp(ext, L".ogg") == 0) {
            char u8path[MAX_PATH_LEN];
            WideCharToMultiByte(CP_UTF8, 0, g_playlist[index].full_path, -1, u8path, sizeof(u8path), NULL, NULL);
            int err = 0;
            stb_vorbis* v = stb_vorbis_open_filename(u8path, &err, NULL);
            if (v) {
                unsigned int samps = stb_vorbis_stream_length_in_samples(v);
                stb_vorbis_info nfo = stb_vorbis_get_info(v);
                if (nfo.sample_rate > 0) {
                    g_current_track_total_sec = (int)(samps / nfo.sample_rate);
                }
                stb_vorbis_close(v);
            }
        }
    }

    save_state(g_current_loaded_dir, index, g_volume, g_repeat_single, g_is_shuffle);

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

    wcsncpy(g_current_loaded_dir, clean_dir, MAX_PATH_LEN - 1);
    g_current_loaded_dir[MAX_PATH_LEN - 1] = L'\0';

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
            if (initial_dir[0] == L'\0') {
                wcsncpy(initial_dir, w_argv[i], MAX_PATH_LEN - 1);
                clean_quotes_w(initial_dir);
            }
        }
    }

    int start_track = 0;
    wchar_t saved_dir[MAX_PATH_LEN] = {0};
    int saved_track = 0;
    float saved_vol = 0.15f;
    bool saved_repeat = false;
    bool saved_shuffle = false;
    bool has_saved_state = load_state(saved_dir, &saved_track, &saved_vol, &saved_repeat, &saved_shuffle);

    if (initial_dir[0] != L'\0') {
        start_track = 0;
    } else if (has_saved_state && GetFileAttributesW(saved_dir) != INVALID_FILE_ATTRIBUTES) {
        wcsncpy(initial_dir, saved_dir, MAX_PATH_LEN - 1);
        initial_dir[MAX_PATH_LEN - 1] = L'\0';
        start_track = saved_track;
        g_volume = saved_vol;
        g_repeat_single = saved_repeat;
        if (saved_shuffle) {
            g_is_shuffle = true;
        }
    } else {
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
        wprintf(L"\n[Error] No audio files (.mp3, .wav, .flac, .ogg, .m4a) found in target path.\n");
        wprintf(L"Press any key to exit...\n");
        _getch();
        if (w_argv) LocalFree(w_argv);
        return 1;
    }

    if (g_is_shuffle) {
        apply_shuffle(true);
    }

    if (start_track < 0 || start_track >= g_playlist_count) {
        start_track = 0;
    }

    InitializeCriticalSection(&g_audio_cs);

    RegisterHotKey(NULL, 1, 0, VK_MEDIA_PLAY_PAUSE);
    RegisterHotKey(NULL, 2, 0, VK_MEDIA_NEXT_TRACK);
    RegisterHotKey(NULL, 3, 0, VK_MEDIA_PREV_TRACK);
    RegisterHotKey(NULL, 4, 0, VK_MEDIA_STOP);

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    wprintf(L"\x1b[2J\x1b[H");
    draw_shortcuts_box();

    play_track(start_track);

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
                    }
                } else if (arrow == 77) {
                    if (g_decoder_init) {
                        EnterCriticalSection(&g_audio_cs);
                        ma_uint64 cur_pcm = 0, len_pcm = 0;
                        ma_decoder_get_cursor_in_pcm_frames(&g_decoder, &cur_pcm);
                        ma_decoder_get_length_in_pcm_frames(&g_decoder, &len_pcm);
                        if (len_pcm == 0 && g_current_track_total_sec > 0) {
                            len_pcm = (ma_uint64)g_current_track_total_sec * g_decoder.outputSampleRate;
                        }
                        ma_uint64 seek_frames = (ma_uint64)g_decoder.outputSampleRate * 5;
                        ma_decoder_seek_to_pcm_frame(&g_decoder, cur_pcm + seek_frames < len_pcm ? cur_pcm + seek_frames : len_pcm);
                        LeaveCriticalSection(&g_audio_cs);
                    }
                } else if (arrow == 72) {
                    g_volume = (g_volume + 0.05f > 1.0f) ? 1.0f : g_volume + 0.05f;
                    if (g_device_init) ma_device_set_master_volume(&g_device, g_volume);
                    save_state(g_current_loaded_dir, g_current_index, g_volume, g_repeat_single, g_is_shuffle);
                } else if (arrow == 80) {
                    g_volume = (g_volume - 0.05f < 0.0f) ? 0.0f : g_volume - 0.05f;
                    if (g_device_init) ma_device_set_master_volume(&g_device, g_volume);
                    save_state(g_current_loaded_dir, g_current_index, g_volume, g_repeat_single, g_is_shuffle);
                }
            } else if (ch == ' ') {
                if (g_device_init) {
                    if (g_is_paused) { ma_device_start(&g_device); g_is_paused = false; }
                    else { ma_device_stop(&g_device); g_is_paused = true; }
                }
            } else if (ch == 'n' || ch == 'N') {
                next_track();
            } else if (ch == 'p' || ch == 'P') {
                prev_track();
            } else if (ch == '+' || ch == '=') {
                g_volume = (g_volume + 0.05f > 1.0f) ? 1.0f : g_volume + 0.05f;
                if (g_device_init) ma_device_set_master_volume(&g_device, g_volume);
                save_state(g_current_loaded_dir, g_current_index, g_volume, g_repeat_single, g_is_shuffle);
            } else if (ch == '-' || ch == '_') {
                g_volume = (g_volume - 0.05f < 0.0f) ? 0.0f : g_volume - 0.05f;
                if (g_device_init) ma_device_set_master_volume(&g_device, g_volume);
                save_state(g_current_loaded_dir, g_current_index, g_volume, g_repeat_single, g_is_shuffle);
            } else if (ch == 'r' || ch == 'R') {
                g_repeat_single = !g_repeat_single;
                save_state(g_current_loaded_dir, g_current_index, g_volume, g_repeat_single, g_is_shuffle);
            } else if (ch == 's' || ch == 'S') {
                apply_shuffle(!g_is_shuffle);
                save_state(g_current_loaded_dir, g_current_index, g_volume, g_repeat_single, g_is_shuffle);
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
                        save_state(g_current_loaded_dir, 0, g_volume, g_repeat_single, g_is_shuffle);
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
                }
            } else if (ch == ']' || ch == '}') {
                if (g_decoder_init) {
                    EnterCriticalSection(&g_audio_cs);
                    ma_uint64 cur_pcm = 0, len_pcm = 0;
                    ma_decoder_get_cursor_in_pcm_frames(&g_decoder, &cur_pcm);
                    ma_decoder_get_length_in_pcm_frames(&g_decoder, &len_pcm);
                    if (len_pcm == 0 && g_current_track_total_sec > 0) {
                        len_pcm = (ma_uint64)g_current_track_total_sec * g_decoder.outputSampleRate;
                    }
                    ma_uint64 seek_frames = (ma_uint64)g_decoder.outputSampleRate * 15;
                    ma_decoder_seek_to_pcm_frame(&g_decoder, cur_pcm + seek_frames < len_pcm ? cur_pcm + seek_frames : len_pcm);
                    LeaveCriticalSection(&g_audio_cs);
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

    save_state(g_current_loaded_dir, g_current_index, g_volume, g_repeat_single, g_is_shuffle);

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
