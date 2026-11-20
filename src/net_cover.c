#include "net_cover.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winhttp.h>

#define CACHE_SIZE 64

typedef struct {
    wchar_t title[260];
    wchar_t artist[260];
    char cover_url[MAX_URL_LEN];
    char album_name[MAX_ALBUM_LEN];
} CacheEntry;

static CacheEntry g_cache[CACHE_SIZE];
static int g_cache_count = 0;
static CRITICAL_SECTION g_cache_cs;

typedef struct {
    wchar_t title[260];
    wchar_t artist[260];
    cover_resolved_cb callback;
} WorkerParam;

void net_cover_init(void) {
    InitializeCriticalSection(&g_cache_cs);
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_count = 0;
}

void net_cover_cleanup(void) {
    DeleteCriticalSection(&g_cache_cs);
}

static bool cache_lookup(const wchar_t* title, const wchar_t* artist, char* out_url, char* out_album) {
    EnterCriticalSection(&g_cache_cs);
    for (int i = 0; i < g_cache_count; i++) {
        if (_wcsicmp(g_cache[i].title, title) == 0 &&
            _wcsicmp(g_cache[i].artist, artist) == 0) {
            strncpy(out_url, g_cache[i].cover_url, MAX_URL_LEN - 1);
            out_url[MAX_URL_LEN - 1] = '\0';
            strncpy(out_album, g_cache[i].album_name, MAX_ALBUM_LEN - 1);
            out_album[MAX_ALBUM_LEN - 1] = '\0';
            LeaveCriticalSection(&g_cache_cs);
            return true;
        }
    }
    LeaveCriticalSection(&g_cache_cs);
    return false;
}

static void cache_insert(const wchar_t* title, const wchar_t* artist, const char* url, const char* album) {
    EnterCriticalSection(&g_cache_cs);
    int idx = 0;
    if (g_cache_count < CACHE_SIZE) {
        idx = g_cache_count++;
    } else {
        memmove(&g_cache[0], &g_cache[1], sizeof(CacheEntry) * (CACHE_SIZE - 1));
        idx = CACHE_SIZE - 1;
    }
    wcsncpy(g_cache[idx].title, title, 259);
    wcsncpy(g_cache[idx].artist, artist, 259);
    strncpy(g_cache[idx].cover_url, url, MAX_URL_LEN - 1);
    strncpy(g_cache[idx].album_name, album, MAX_ALBUM_LEN - 1);
    LeaveCriticalSection(&g_cache_cs);
}

static void url_encode_w(const wchar_t* src, wchar_t* dst, size_t dst_max) {
    size_t di = 0;
    for (size_t si = 0; src[si] != L'\0' && di + 4 < dst_max; si++) {
        wchar_t ch = src[si];
        if (iswalnum(ch) || ch == L'-' || ch == L'_' || ch == L'.' || ch == L'~') {
            dst[di++] = ch;
        } else if (ch == L' ') {
            dst[di++] = L'+';
        } else {
            int high = (ch >> 4) & 0x0F;
            int low = ch & 0x0F;
            dst[di++] = L'%';
            dst[di++] = (wchar_t)(high < 10 ? L'0' + high : L'A' + (high - 10));
            dst[di++] = (wchar_t)(low < 10 ? L'0' + low : L'A' + (low - 10));
        }
    }
    dst[di] = L'\0';
}

static DWORD WINAPI cover_worker_thread(LPVOID arg) {
    WorkerParam* p = (WorkerParam*)arg;
    if (!p) return 0;

    char resolved_url[MAX_URL_LEN] = "app_logo";
    char resolved_album[MAX_ALBUM_LEN] = "WaveDar";

    wchar_t combined_query[512];
    _snwprintf(combined_query, 512, L"%s %s", p->artist, p->title);

    wchar_t encoded_query[1024];
    url_encode_w(combined_query, encoded_query, 1024);

    wchar_t request_path[2048];
    _snwprintf(request_path, 2048, L"/search?term=%s&media=music&entity=song&limit=1", encoded_query);

    HINTERNET hSession = WinHttpOpen(L"WaveDar/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (hSession) {
        WinHttpSetTimeouts(hSession, 3000, 3000, 3000, 3000);

        HINTERNET hConnect = WinHttpConnect(hSession, L"itunes.apple.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", request_path,
                                                   NULL, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   WINHTTP_FLAG_SECURE);
            if (hRequest) {
                if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                    WinHttpReceiveResponse(hRequest, NULL)) {

                    char json_buf[8192];
                    DWORD bytes_read = 0;
                    if (WinHttpReadData(hRequest, json_buf, sizeof(json_buf) - 1, &bytes_read) && bytes_read > 0) {
                        json_buf[bytes_read] = '\0';

                        const char* art_key = "\"artworkUrl100\":\"";
                        char* art_pos = strstr(json_buf, art_key);
                        if (art_pos) {
                            art_pos += strlen(art_key);
                            char* art_end = strchr(art_pos, '\"');
                            if (art_end) {
                                size_t ulen = art_end - art_pos;
                                if (ulen < MAX_URL_LEN - 1) {
                                    strncpy(resolved_url, art_pos, ulen);
                                    resolved_url[ulen] = '\0';

                                    char* res_pos = strstr(resolved_url, "100x100bb.jpg");
                                    if (res_pos) {
                                        memcpy(res_pos, "512x512bb.jpg", 13);
                                    }
                                }
                            }
                        }

                        const char* col_key = "\"collectionName\":\"";
                        char* col_pos = strstr(json_buf, col_key);
                        if (col_pos) {
                            col_pos += strlen(col_key);
                            char* col_end = strchr(col_pos, '\"');
                            if (col_end) {
                                size_t clen = col_end - col_pos;
                                if (clen < MAX_ALBUM_LEN - 1) {
                                    strncpy(resolved_album, col_pos, clen);
                                    resolved_album[clen] = '\0';
                                }
                            }
                        }
                    }
                }
                WinHttpCloseHandle(hRequest);
            }
            WinHttpCloseHandle(hConnect);
        }
        WinHttpCloseHandle(hSession);
    }

    cache_insert(p->title, p->artist, resolved_url, resolved_album);

    if (p->callback) {
        p->callback(p->title, resolved_url, resolved_album);
    }

    free(p);
    return 0;
}

void net_cover_resolve_async(const wchar_t* title, const wchar_t* artist, cover_resolved_cb callback) {
    if (!title || !callback) return;

    char cached_url[MAX_URL_LEN];
    char cached_album[MAX_ALBUM_LEN];
    if (cache_lookup(title, artist, cached_url, cached_album)) {
        callback(title, cached_url, cached_album);
        return;
    }

    WorkerParam* p = (WorkerParam*)malloc(sizeof(WorkerParam));
    if (!p) return;

    wcsncpy(p->title, title, 259);
    p->title[259] = L'\0';
    wcsncpy(p->artist, artist, 259);
    p->artist[259] = L'\0';
    p->callback = callback;

    HANDLE hThread = CreateThread(NULL, 0, cover_worker_thread, p, 0, NULL);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        free(p);
    }
}
