// Copyright (c) 2026 CallMeLewis
// The track the phone plays, from its media status channel's metadata message (0x8003): pure, tested on the PC.
//
// The message, as open Android Auto implementations read it (aasdk's MediaInfoChannelMetadataData, the Desktop Head
// Unit's MediaPlaybackMetadata): 1 song, 2 artist, 3 album (strings), 4 album art (the picture's bytes, PNG or JPEG),
// 5 playlist name, 6 duration in seconds. The phone sends no list of tracks: browsing stays on the phone.
// tests/test_media_metadata.cpp.
#ifndef AA_ENTRYNAV_CORE_MEDIA_METADATA_HPP
#define AA_ENTRYNAV_CORE_MEDIA_METADATA_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "pb.hpp"

namespace aa {

struct TrackInfo {
    std::string title, artist, album, playlist;
    std::string art;   // the album art's bytes; empty without one, or past kTrackArtMax
    uint32_t artHash;  // FNV-1a of art, 0 without one: tells a new picture from the same one
    uint32_t durationS;
    TrackInfo() : artHash(0), durationS(0) {}
    bool sameText(const TrackInfo& other) const {
        return title == other.title && artist == other.artist && album == other.album && playlist == other.playlist;
    }
    // What the iDrive app shows: the texts and the picture (the duration alone does not count).
    bool sameShown(const TrackInfo& other) const { return sameText(other) && artHash == other.artHash; }
};

// Each text is cut at this many bytes: the cluster shows one short line, and the file the app reads stays small.
const size_t kTrackTextMax = 200;
// A larger album art is left out: phones send a few tens of kilobytes.
const size_t kTrackArtMax = 1024 * 1024;

inline uint32_t trackArtHash(const uint8_t* data, size_t size) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        h = (h ^ data[i]) * 16777619u;
    }
    return h ? h : 1;
}

// The metadata message's body (after its id) into info. False when it is malformed; info then holds what came before.
inline bool parseMediaMetadata(const uint8_t* data, size_t size, TrackInfo& info) {
    info = TrackInfo();
    pb::FieldReader reader(data, size);
    pb::Field field;
    while (reader.next(field)) {
        std::string* text = field.number == 1   ? &info.title
                            : field.number == 2 ? &info.artist
                            : field.number == 3 ? &info.album
                            : field.number == 5 ? &info.playlist
                                                : nullptr;
        if (text && field.type == pb::kBytes) {
            text->assign(reinterpret_cast<const char*>(field.data),
                         field.size < kTrackTextMax ? field.size : kTrackTextMax);
        } else if (field.number == 4 && field.type == pb::kBytes && field.size > 0 && field.size <= kTrackArtMax) {
            info.art.assign(reinterpret_cast<const char*>(field.data), field.size);
            info.artHash = trackArtHash(field.data, field.size);
        } else if (field.number == 6 && field.type == pb::kVarint) {
            info.durationS = static_cast<uint32_t>(field.value);
        }
    }
    return !reader.malformed();
}

}  // namespace aa

#endif  // AA_ENTRYNAV_CORE_MEDIA_METADATA_HPP
