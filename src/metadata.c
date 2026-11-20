#include "metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <windows.h>

static void trim_wstr(wchar_t* str) {
    if (!str || !*str) return;
    wchar_t* start = str;
    while (*start && iswspace(*start)) start++;
    if (start != str) {
        memmove(str, start, (wcslen(start) + 1) * sizeof(wchar_t));
    }
    size_t len = wcslen(str);
    while (len > 0 && iswspace(str[len - 1])) {
        str[--len] = L'\0';
    }
}

static uint32_t read_syncsafe_uint32(const uint8_t* b) {
    return ((uint32_t)(b[0] & 0x7F) << 21) |
           ((uint32_t)(b[1] & 0x7F) << 14) |
           ((uint32_t)(b[2] & 0x7F) << 7)  |
           ((uint32_t)(b[3] & 0x7F));
}

static bool parse_id3v2_tag(const wchar_t* path, wchar_t* out_title, wchar_t* out_artist) {
    FILE* fp = _wfopen(path, L"rb");
    if (!fp) return false;

    uint8_t header[10];
    if (fread(header, 1, 10, fp) != 10) {
        fclose(fp);
        return false;
    }

    if (memcmp(header, "ID3", 3) != 0) {
        fclose(fp);
        return false;
    }

    uint8_t version = header[3];
    uint32_t tag_size = read_syncsafe_uint32(&header[6]);
    if (tag_size == 0 || tag_size > 1024 * 1024) {
        fclose(fp);
        return false;
    }

    uint32_t read_cap = tag_size > 65536 ? 65536 : tag_size;
    uint8_t* buf = (uint8_t*)malloc(read_cap);
    if (!buf) {
        fclose(fp);
        return false;
    }

    size_t bytes_read = fread(buf, 1, read_cap, fp);
    fclose(fp);

    bool found_title = false;
    bool found_artist = false;
    size_t offset = 0;

    while (offset + 10 < bytes_read) {
        char frame_id[5] = {0};
        memcpy(frame_id, &buf[offset], 4);

        if (frame_id[0] < 'A' || frame_id[0] > 'Z') break;

        uint32_t frame_size = 0;
        if (version == 4) {
            frame_size = read_syncsafe_uint32(&buf[offset + 4]);
        } else {
            frame_size = ((uint32_t)buf[offset + 4] << 24) |
                         ((uint32_t)buf[offset + 5] << 16) |
                         ((uint32_t)buf[offset + 6] << 8)  |
                         ((uint32_t)buf[offset + 7]);
        }

        offset += 10;
        if (frame_size == 0 || offset + frame_size > bytes_read) break;

        uint8_t encoding = buf[offset];
        const uint8_t* payload = &buf[offset + 1];
        size_t payload_len = frame_size > 1 ? frame_size - 1 : 0;

        wchar_t* dest_buf = NULL;
        bool* dest_found = NULL;

        if (strcmp(frame_id, "TIT2") == 0 && !found_title) {
            dest_buf = out_title;
            dest_found = &found_title;
        } else if (strcmp(frame_id, "TPE1") == 0 && !found_artist) {
            dest_buf = out_artist;
            dest_found = &found_artist;
        }

        if (dest_buf && payload_len > 0) {
            if (encoding == 1 || encoding == 2) {
                // UTF-16 with BOM or big-endian
                size_t wchars = payload_len / sizeof(wchar_t);
                if (wchars >= MAX_TITLE_LEN) wchars = MAX_TITLE_LEN - 1;
                wcsncpy(dest_buf, (const wchar_t*)payload, wchars);
                dest_buf[wchars] = L'\0';
                trim_wstr(dest_buf);
                *dest_found = (wcslen(dest_buf) > 0);
            } else if (encoding == 3) {
                // UTF-8
                int cch = MultiByteToWideChar(CP_UTF8, 0, (const char*)payload, (int)payload_len, dest_buf, MAX_TITLE_LEN - 1);
                if (cch > 0) {
                    dest_buf[cch] = L'\0';
                    trim_wstr(dest_buf);
                    *dest_found = (wcslen(dest_buf) > 0);
                }
            } else {
                // ISO-8859-1 / ASCII
                int cch = MultiByteToWideChar(CP_ACP, 0, (const char*)payload, (int)payload_len, dest_buf, MAX_TITLE_LEN - 1);
                if (cch > 0) {
                    dest_buf[cch] = L'\0';
                    trim_wstr(dest_buf);
                    *dest_found = (wcslen(dest_buf) > 0);
                }
            }
        }

        offset += frame_size;
        if (found_title && found_artist) break;
    }

    free(buf);
    return (found_title && found_artist);
}

static void sanitize_from_filename(const wchar_t* filename_base, wchar_t* out_title, wchar_t* out_artist) {
    wchar_t temp[MAX_TITLE_LEN * 2];
    wcsncpy(temp, filename_base, (sizeof(temp) / sizeof(wchar_t)) - 1);
    temp[(sizeof(temp) / sizeof(wchar_t)) - 1] = L'\0';

    // Strip trailing file extension
    wchar_t* dot = wcsrchr(temp, L'.');
    if (dot && (_wcsicmp(dot, L".mp3") == 0 || _wcsicmp(dot, L".wav") == 0 ||
                _wcsicmp(dot, L".flac") == 0 || _wcsicmp(dot, L".ogg") == 0 ||
                _wcsicmp(dot, L".m4a") == 0)) {
        *dot = L'\0';
    }

    // Strip leading track numbers like "01. ", "01 - ", "[1] "
    wchar_t* p = temp;
    while (*p && (iswdigit(*p) || *p == L' ' || *p == L'.' || *p == L'-' || *p == L'[' || *p == L']')) {
        if (*p == L'.' || *p == L'-' || *p == L']') {
            p++;
            while (*p == L' ') p++;
            break;
        }
        p++;
    }
    if (*p != L'\0') {
        memmove(temp, p, (wcslen(p) + 1) * sizeof(wchar_t));
    }

    // Remove YouTube converter prefixes/suffixes
    const wchar_t* junk_patterns[] = {
        L"yt1s.com - ", L"yt1s.com ",
        L" [Official Audio]", L" [Official Music Video]",
        L" (Official Audio)", L" (Official Music Video)",
        L" [Official Video]", L" (Official Video)",
        L" (Lyrics)", L" [Lyrics]",
        L" (Lyric Video)", L" [Lyric Video]",
        NULL
    };

    for (int i = 0; junk_patterns[i] != NULL; i++) {
        wchar_t* found = wcsstr(temp, junk_patterns[i]);
        if (found) {
            size_t plen = wcslen(junk_patterns[i]);
            memmove(found, found + plen, (wcslen(found + plen) + 1) * sizeof(wchar_t));
        }
    }

    // Split on " - " into Artist and Title
    wchar_t* sep = wcsstr(temp, L" - ");
    if (sep) {
        *sep = L'\0';
        wcsncpy(out_artist, temp, MAX_ARTIST_LEN - 1);
        out_artist[MAX_ARTIST_LEN - 1] = L'\0';
        trim_wstr(out_artist);

        wcsncpy(out_title, sep + 3, MAX_TITLE_LEN - 1);
        out_title[MAX_TITLE_LEN - 1] = L'\0';
        trim_wstr(out_title);
    } else {
        wcsncpy(out_title, temp, MAX_TITLE_LEN - 1);
        out_title[MAX_TITLE_LEN - 1] = L'\0';
        trim_wstr(out_title);

        wcscpy(out_artist, L"Local Artist");
    }
}

void extract_track_metadata(const wchar_t* file_path, const wchar_t* filename_base, TrackMetadata* out_meta) {
    if (!out_meta) return;

    memset(out_meta, 0, sizeof(TrackMetadata));

    // Priority 1: ID3v2 Tags
    if (file_path && parse_id3v2_tag(file_path, out_meta->title, out_meta->artist)) {
        out_meta->has_tags = true;
        return;
    }

    // Priority 2: Sanitized filename
    sanitize_from_filename(filename_base, out_meta->title, out_meta->artist);
    out_meta->has_tags = false;
}
