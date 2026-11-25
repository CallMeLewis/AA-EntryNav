// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// What the phone is told about this car must stay byte for byte the golden description (vehicleId included).
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "dhu_sdp.h"
#include "evo_gps.hpp"
#include "pb.hpp"
#include "sdr_builder.hpp"
#include "sdr_g20_golden.hpp"

namespace {

// With the car's GNSS ring (a fixture of its size) or without it (no file).
bool matches(bool carGps, const uint8_t* golden, size_t goldenLen) {
    const char* sensors = carGps ? "with car GPS" : "without car GPS";
    char ring[96];
    snprintf(ring, sizeof(ring), "/tmp/entry-sdr-g20-ring-%d", int(getpid()));
    unlink(ring);
    if (carGps) {
        FILE* f = fopen(ring, "wb");
        if (!f || ftruncate(fileno(f), 0x4258) != 0) {
            return false;
        }
        fclose(f);
    }
    evo::setCarGpsSourceForTest(ring);
    const std::vector<uint8_t> sdr = aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr);
    if (sdr.size() != goldenLen) {
        printf("FAIL sensors %s: %u bytes, %u expected\n", sensors, static_cast<unsigned>(sdr.size()),
               static_cast<unsigned>(goldenLen));
        return false;
    }
    for (size_t i = 0; i < sdr.size(); ++i) {
        if (sdr[i] != golden[i]) {
            printf("FAIL sensors %s: byte %u differs\n", sensors, static_cast<unsigned>(i));
            return false;
        }
    }
    unlink(ring);
    return true;
}

// The first sub-message `number` of a message; false when absent.
bool sub(const uint8_t* data, size_t size, uint32_t number, const uint8_t*& out, size_t& outSize) {
    aa::pb::FieldReader reader(data, size);
    aa::pb::Field field;
    while (reader.next(field)) {
        if (aa::pb::isMessage(field, number)) {
            out = field.data;
            outSize = field.size;
            return true;
        }
    }
    return false;
}

// The service (a top-level field 1) of a channel; false when absent.
bool service(const std::vector<uint8_t>& sdr, uint64_t channel, const uint8_t*& out, size_t& outSize) {
    aa::pb::FieldReader reader(sdr.data(), sdr.size());
    aa::pb::Field field;
    uint64_t id = 0;
    while (reader.next(field)) {
        if (aa::pb::isMessage(field, 1) && aa::pb::findVarint(field.data, field.size, 1, id) && id == channel) {
            out = field.data;
            outSize = field.size;
            return true;
        }
    }
    return false;
}

// What the phone reads of the main display and the touchscreen.
struct Display {
    uint64_t resolution, density, realDensity, bottomInset, touchWidth, touchHeight;
};
bool display(const std::vector<uint8_t>& sdr, Display& d) {
    const uint8_t *video, *sink, *config, *ui, *insets, *input, *source, *touch;
    size_t videoN, sinkN, configN, uiN, insetsN, inputN, sourceN, touchN;
    return service(sdr, 6, video, videoN) && sub(video, videoN, 3, sink, sinkN) &&
           sub(sink, sinkN, 4, config, configN) && aa::pb::findVarint(config, configN, 1, d.resolution) &&
           aa::pb::findVarint(config, configN, 5, d.density) && aa::pb::findVarint(config, configN, 9, d.realDensity) &&
           sub(config, configN, 11, ui, uiN) && sub(ui, uiN, 1, insets, insetsN) &&
           aa::pb::findVarint(insets, insetsN, 2, d.bottomInset) && service(sdr, 7, input, inputN) &&
           sub(input, inputN, 4, source, sourceN) && sub(source, sourceN, 2, touch, touchN) &&
           aa::pb::findVarint(touch, touchN, 1, d.touchWidth) && aa::pb::findVarint(touch, touchN, 2, d.touchHeight);
}

// The 1080p description: the golden one but for the canvas's six values (each the same length on the wire).
bool hdMatches() {
    evo::setCarGpsSourceForTest("/nonexistent");
    const std::vector<uint8_t> golden(kSdrG20WithoutCarGps, kSdrG20WithoutCarGps + sizeof(kSdrG20WithoutCarGps));
    const std::vector<uint8_t> hd =
        aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, false, aa::canvasFor(true));
    Display sd, hdd;
    if (!display(golden, sd) || !display(hd, hdd)) {
        printf("FAIL 1080p: display or touchscreen not found\n");
        return false;
    }
    if (sd.resolution != 2 || sd.density != 140 || sd.realDensity != 140 || sd.bottomInset != 240 ||
        sd.touchWidth != 1280 || sd.touchHeight != 720) {
        printf("FAIL 720p golden read back wrongly\n");
        return false;
    }
    if (hdd.resolution != 3 || hdd.density != 210 || hdd.realDensity != 210 || hdd.bottomInset != 360 ||
        hdd.touchWidth != 1920 || hdd.touchHeight != 1080) {
        printf("FAIL 1080p: resolution %u, density %u/%u, inset %u, touchscreen %ux%u\n",
               static_cast<unsigned>(hdd.resolution), static_cast<unsigned>(hdd.density),
               static_cast<unsigned>(hdd.realDensity), static_cast<unsigned>(hdd.bottomInset),
               static_cast<unsigned>(hdd.touchWidth), static_cast<unsigned>(hdd.touchHeight));
        return false;
    }
    unsigned differ = 0;
    for (size_t i = 0; i < hd.size() && i < golden.size(); ++i) {
        differ += hd[i] != golden[i];
    }
    if (hd.size() != golden.size() || differ > 8) {
        printf("FAIL 1080p: %u bytes (%u expected), %u differ (at most 8)\n", static_cast<unsigned>(hd.size()),
               static_cast<unsigned>(golden.size()), differ);
        return false;
    }
    return true;
}

// Other screens: the canvas each gets, and the description the phone is told for it. A phone on Wi-Fi gets no canvas
// wider than 1280: the 1440x540 screen gets 1280x720 stretched to it, at the density that keeps the cable's layout.
bool smallScreens() {
    struct Case {
        uint32_t panelW, panelH;
        bool hd;
        uint32_t widest, resolution, width, height, shownHeight, density;
    };
    const uint32_t any = aa::kWidestCanvas, wifi = aa::kWifiCanvasWidth;
    const Case cases[] = {
        {1280, 480, false, any, 2, 1280, 720, 480, 140},  {1280, 480, true, any, 3, 1920, 1080, 720, 210},
        {800, 480, false, any, 1, 800, 480, 480, 140},    {800, 480, true, any, 1, 800, 480, 480, 140},
        {800, 400, false, any, 1, 800, 480, 400, 140},    {800, 400, true, any, 2, 1280, 720, 640, 224},
        {1440, 540, false, any, 3, 1920, 1080, 720, 186}, {1440, 540, true, any, 3, 1920, 1080, 720, 186},
        {1280, 480, false, wifi, 2, 1280, 720, 480, 140}, {1280, 480, true, wifi, 2, 1280, 720, 480, 140},
        {800, 400, false, wifi, 1, 800, 480, 400, 140},   {800, 400, true, wifi, 2, 1280, 720, 640, 224},
        {1440, 540, false, wifi, 2, 1280, 720, 480, 124}, {1440, 540, true, wifi, 2, 1280, 720, 480, 124},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const Case& c = cases[i];
        aa::Canvas canvas;
        if (!aa::canvasFor(c.panelW, c.panelH, c.hd, canvas, c.widest) || canvas.resolution != c.resolution ||
            canvas.width != c.width || canvas.height != c.height || canvas.shownHeight != c.shownHeight ||
            canvas.density != c.density) {
            printf("FAIL %ux%u%s: canvas %u %ux%u, %u rows, density %u\n", c.panelW, c.panelH, c.hd ? " hd" : "",
                   canvas.resolution, canvas.width, canvas.height, canvas.shownHeight, canvas.density);
            return false;
        }
        Display d;
        if (!display(aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, false, canvas), d) ||
            d.resolution != c.resolution || d.density != c.density || d.realDensity != c.density ||
            d.bottomInset != c.height - c.shownHeight || d.touchWidth != c.width || d.touchHeight != c.height) {
            printf("FAIL %ux%u%s: description read back wrongly\n", c.panelW, c.panelH, c.hd ? " hd" : "");
            return false;
        }
    }
    // Screens no canvas fits: too wide, or rows that don't come out whole. Wi-Fi stretches none of them.
    const uint32_t refused[][2] = {{2560, 480}, {1024, 600}, {1000, 480}, {0, 480}, {1280, 0}};
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        aa::Canvas canvas;
        if (aa::canvasFor(refused[i][0], refused[i][1], false, canvas) ||
            aa::canvasFor(refused[i][0], refused[i][1], true, canvas) ||
            aa::canvasFor(refused[i][0], refused[i][1], false, canvas, aa::kWifiCanvasWidth)) {
            printf("FAIL %ux%u: a canvas was given\n", refused[i][0], refused[i][1]);
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    // With the car GPS (LOCATION, NIGHT_MODE, DRIVING_STATUS) and without it.
    if (!matches(true, kSdrG20WithCarGps, sizeof(kSdrG20WithCarGps)) ||
        !matches(false, kSdrG20WithoutCarGps, sizeof(kSdrG20WithoutCarGps)) || !hdMatches() || !smallScreens()) {
        return 1;
    }
    printf(
        "PASS car description identical to the golden record (%u bytes with car GPS, %u without); only the head "
        "unit name and build change; 1080p changes only the resolution, densities, inset and touchscreen; 800x480 "
        "and 800x400 screens get their canvas, 1440x540 gets 1280x720 stretched on Wi-Fi, screens none fits get none\n",
        static_cast<unsigned>(sizeof(kSdrG20WithCarGps)), static_cast<unsigned>(sizeof(kSdrG20WithoutCarGps)));
    return 0;
}
