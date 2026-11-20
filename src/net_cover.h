#ifndef NET_COVER_H
#define NET_COVER_H

#include <wchar.h>
#include <stdbool.h>

#define MAX_URL_LEN 512
#define MAX_ALBUM_LEN 260

typedef void (*cover_resolved_cb)(const wchar_t* title, const char* cover_url, const char* album_name);

void net_cover_init(void);
void net_cover_cleanup(void);

// Resolve cover art in a detached background thread without blocking audio playback.
void net_cover_resolve_async(const wchar_t* title, const wchar_t* artist, cover_resolved_cb callback);

#endif // NET_COVER_H
