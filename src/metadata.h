#ifndef METADATA_H
#define METADATA_H

#include <wchar.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_TITLE_LEN 260
#define MAX_ARTIST_LEN 260

typedef struct {
    wchar_t title[MAX_TITLE_LEN];
    wchar_t artist[MAX_ARTIST_LEN];
    bool has_tags;
} TrackMetadata;

// Extract metadata from file: ID3v2 tags first, then fallback to filename sanitizer
void extract_track_metadata(const wchar_t* file_path, const wchar_t* filename_base, TrackMetadata* out_meta);

#endif // METADATA_H
