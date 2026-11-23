// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The picture the phone draws for the car's screen, and the part of it the screen shows.
#ifndef AA_ENTRYNAV_CORE_CANVAS_HPP
#define AA_ENTRYNAV_CORE_CANVAS_HPP

#include <stdint.h>

namespace aa {

// The reference car's screen.
const uint32_t kPanelWidth = 1280, kPanelHeight = 480;

// Android Auto draws 800x480, 1280x720 or 1920x1080. The screen shows the canvas's full width and its top shownHeight
// rows, scaled to the screen when the canvas is wider; the rows below are the bottom inset, where the phone draws
// nothing. The standard canvas is the smallest one at least as wide as the screen whose rows fill it at that scale.
// The hd canvas is the next one that fits, shrunk onto the screen by the car's compositor: the phone draws more detail,
// so diagonal edges come out smoother and colours bleed less around thin lines. Density grows with the scale, so every
// button keeps its size on the screen.
//   1280x480   1280x720, top 480 rows 1:1, density 140 (seen on the car: the rail icons no longer overlap)
//              hd: 1920x1080, top 720 rows, density 210
//   800x480    800x480 whole, density 140; no hd (1280x720 would need 768 rows)
//   800x400    800x480, top 400 rows, density 140; hd: 1280x720, top 640 rows, density 224
struct Canvas {
    uint32_t resolution;     // the VideoConfig's code: 1 for 800x480, 2 for 1280x720, 3 for 1920x1080
    uint32_t width, height;  // also the touchscreen's size: touches arrive in canvas pixels
    uint32_t shownHeight;    // rows shown on the screen; the rest is the bottom inset
    uint32_t density;
};

// The canvas for a panelWidth x panelHeight screen: the hd one when asked for and the screen has one, else the
// standard one. False when no canvas fits the screen.
inline bool canvasFor(uint32_t panelWidth, uint32_t panelHeight, bool hd, Canvas& out) {
    static const Canvas kAll[] = {{1, 800, 480, 0, 0}, {2, 1280, 720, 0, 0}, {3, 1920, 1080, 0, 0}};
    if (!panelWidth || !panelHeight) {
        return false;
    }
    bool found = false;
    for (unsigned i = 0; i < sizeof(kAll) / sizeof(kAll[0]); ++i) {
        const Canvas& c = kAll[i];
        const uint64_t rows = static_cast<uint64_t>(c.width) * panelHeight;
        if (c.width < panelWidth || rows % panelWidth || rows / panelWidth > c.height) {
            continue;
        }
        out = c;
        out.shownHeight = static_cast<uint32_t>(rows / panelWidth);
        out.density = 140 * c.width / panelWidth;
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
