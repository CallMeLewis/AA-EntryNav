// Copyright (c) 2026 CallMeLewis
// The phone's track metadata (core/media_metadata.hpp): its texts, duration and album art, odd input survived.
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "media_metadata.hpp"

#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                             \
        }                                                        \
    } while (0)

int main() {
    using aa::pb::appendBytes;
    using aa::pb::appendString;
    using aa::pb::appendUint;
    {  // A whole message: texts, a PNG in field 4, the duration, and a field this reader does not know
        std::vector<uint8_t> m;
        appendString(m, 1, "Strobe");
        appendString(m, 2, "deadmau5");
        appendString(m, 3, "For Lack of a Better Name");
        const uint8_t png[] = {0x89, 'P', 'N', 'G', 0, 1, 2, 3};
        appendBytes(m, 4, png, sizeof(png));
        appendString(m, 5, "Drive");
        appendUint(m, 6, 637);
        appendUint(m, 7, 3);
        aa::TrackInfo t;
        CHECK(aa::parseMediaMetadata(&m[0], m.size(), t));
        CHECK(t.title == "Strobe" && t.artist == "deadmau5" && t.album == "For Lack of a Better Name");
        CHECK(t.playlist == "Drive" && t.durationS == 637);
        CHECK(t.art.size() == sizeof(png) && t.art[1] == 'P' && t.artHash == aa::trackArtHash(png, sizeof(png)));
    }
    {  // UTF-8 passes through as it is; an overlong text is cut
        std::vector<uint8_t> m;
        appendString(m, 1,
                     "Ma\xc3\xb1"
                     "ana \xe2\x80\x93 \xe6\x97\xa5");
        appendString(m, 2, std::string(500, 'x'));
        aa::TrackInfo t;
        CHECK(aa::parseMediaMetadata(&m[0], m.size(), t));
        CHECK(t.title ==
                  "Ma\xc3\xb1"
                  "ana \xe2\x80\x93 \xe6\x97\xa5" &&
              t.artist.size() == aa::kTrackTextMax);
    }
    {  // Cut short: malformed, and what came before is kept; an empty message is a track with nothing known
        std::vector<uint8_t> m;
        appendString(m, 1, "Title");
        appendString(m, 2, "Artist");
        m.resize(m.size() - 2);
        aa::TrackInfo t;
        CHECK(!aa::parseMediaMetadata(&m[0], m.size(), t) && t.title == "Title" && t.artist.empty());
        CHECK(aa::parseMediaMetadata(nullptr, 0, t) && t.title.empty() && t.durationS == 0);
    }
    {  // The same texts compare equal whatever the duration; a new picture is a change to show
        aa::TrackInfo a, b;
        a.title = b.title = "x";
        b.durationS = 9;
        CHECK(a.sameText(b) && a.sameShown(b));
        b.artHash = 7;
        CHECK(a.sameText(b) && !a.sameShown(b));
        b.album = "y";
        CHECK(!a.sameText(b));
    }
    {  // No album art, or one past the limit, leaves art empty and its hash 0
        std::vector<uint8_t> m;
        appendString(m, 1, "Song");
        appendBytes(m, 4, reinterpret_cast<const uint8_t*>(""), 0);
        aa::TrackInfo t;
        CHECK(aa::parseMediaMetadata(&m[0], m.size(), t) && t.art.empty() && t.artHash == 0);
    }
    puts("PASS media metadata: song, artist, album, playlist, duration and album art read, cut input survived");
    return 0;
}
