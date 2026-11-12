// Copyright (c) 2026 CallMeLewis
// The Flare Drive row of the app's menu: a PIN typed with the iDrive's speller (in its masked PIN mode), then a page
// with a QR code that tells Flare Drive the car's Wi-Fi and the head unit's address (flare_link.hpp). The first time,
// the PIN is chosen and typed twice. Wrong PINs lock the page (pin_lock.hpp). The code is hidden while the car moves,
// and dropped when the page is left: the next visit asks for the PIN again.
//
// States, actions and models: rhmi_ids.hpp. Everything runs on the app thread.
#ifndef AA_ENTRYNAV_HMIAPP_FLARE_PAGE_HPP
#define AA_ENTRYNAV_HMIAPP_FLARE_PAGE_HPP

#include "bmw_framework.hpp"

namespace evo {
namespace app {

// Registers the row and the speller's OK.
void registerFlarePage(bmw::PluginApp* app);
// iDrive's focus on the page, and on the app's menu (backing out to it drops a half-chosen PIN).
void flarePageFocus(bmw::PluginApp* app, bool focused);
void flareMenuFocus(bool focused);
// From the status poll, every second: the code hides as the car moves off, and shows again once it stops.
void flarePagePoll(bmw::PluginApp* app);

}  // namespace app
}  // namespace evo

#endif
