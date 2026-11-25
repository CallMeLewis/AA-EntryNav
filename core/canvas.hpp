// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The picture the phone draws for the car's screen, and the part of it the screen shows.
#ifndef AA_ENTRYNAV_CORE_CANVAS_HPP
#define AA_ENTRYNAV_CORE_CANVAS_HPP

#include <stdint.h>

namespace aa {

// The reference car's screen.
const uint32_t kPanelWidth = 1280, kPanelHeight = 480;

// Android Auto draws 800x480, 1280x720 or 1920x1080. The screen shows the canvas's full width and its top shownHeight
// rows, scaled to the screen when their widths differ; the rows below are the bottom inset, where the phone draws
// nothing. The standard canvas is the smallest one at least as wide as the screen whose rows fill it at that scale.
// The hd canvas is the next one that fits, shrunk onto the screen by the car's compositor: the phone draws more detail,
// so diagonal edges come out smoother and colours bleed less around thin lines. Density grows with the scale, so every
// button keeps its size on the screen.
//   1280x480   1280x720, top 480 rows 1:1, density 140 (seen on the car: the rail icons no longer overlap)
//              hd: 1920x1080, top 720 rows, density 210
//   800x480    800x480 whole, density 140; no hd (1280x720 would need 768 rows)
//   800x400    800x480, top 400 rows, density 140; hd: 1280x720, top 640 rows, density 224
//   1440x540   1920x1080, top 720 rows, density 186; on Wi-Fi 1280x720, top 480 rows stretched, density 124
struct Canvas {
    uint32_t resolution;     // the VideoConfig's code: 1 for 800x480, 2 for 1280x720, 3 for 1920x1080
    uint32_t width, height;  // also the touchscreen's size: touches arrive in canvas pixels
    uint32_t shownHeight;    // rows shown on the screen; the rest is the bottom inset
    uint32_t density;
};

const uint32_t kWidestCanvas = 1920;
// The widest canvas a phone on the car's Wi-Fi is asked for. On the reference car's 2.4 GHz hotspot, a phone asked for
// 1920x1080 set the session up, never started the picture and hung up within seconds; 1280x720 works. A screen wider
// than 1280 gets 1280x720 stretched to it on Wi-Fi: its full width and the rows that keep the screen's shape. Upstream
// saw the same on a 1440x540 screen (Jiraiya43, #180). The density follows the rule above, so the layout and the
// buttons' size on the screen match those the cable gives the same screen.
const uint32_t kWifiCanvasWidth = 1280;

// The canvas's full width on a panelWidth x panelHeight screen and the rows that keep its shape: false unless they come
// out whole and the canvas has them.
inline bool placeCanvas(const Canvas& c, uint32_t panelWidth, uint32_t panelHeight, Canvas& out) {
    const uint64_t rows = static_cast<uint64_t>(c.width) * panelHeight;
    if (rows % panelWidth || rows / panelWidth > c.height) {
        return false;
    }
    out = c;
    out.shownHeight = static_cast<uint32_t>(rows / panelWidth);
    out.density = 140 * c.width / panelWidth;
    return true;
}

// The canvas for a panelWidth x panelHeight screen: the hd one when asked for and the screen has one, else the
// standard one. None is wider than widest: when the screen's only canvases are wider, the widest one allowed is
// stretched to it instead. False when no canvas fits the screen.
inline bool canvasFor(uint32_t panelWidth, uint32_t panelHeight, bool hd, Canvas& out,
                      uint32_t widest = kWidestCanvas) {
    static const Canvas kAll[] = {{1, 800, 480, 0, 0}, {2, 1280, 720, 0, 0}, {3, 1920, 1080, 0, 0}};
    if (!panelWidth || !panelHeight) {
        return false;
    }
    bool found = false;
    for (unsigned i = 0; i < sizeof(kAll) / sizeof(kAll[0]); ++i) {
        Canvas placed;
        if (kAll[i].width < panelWidth || !placeCanvas(kAll[i], panelWidth, panelHeight, placed)) {
            continue;
        }
        if (kAll[i].width > widest) {
            if (found) {
                return true;  // the hd canvas would be too wide: the standard one
            }
            for (unsigned k = i; k-- > 0;) {
                if (kAll[k].width <= widest) {
                    return placeCanvas(kAll[k], panelWidth, panelHeight, out);
                }
            }
            return false;
        }
        out = placed;
        if (found || !hd) {
            return true;
        }
        found = true;  // the standard canvas: kept unless a larger one fits
    }
    return found;
}

// The reference screen's canvas.
inline Canvas canvasFor(bool hd) {
    Canvas c = Canvas();
    canvasFor(kPanelWidth, kPanelHeight, hd, c);
    return c;
}

}  // namespace aa

#endif  // AA_ENTRYNAV_CORE_CANVAS_HPP
