// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The app's log, /var/local/data/AA/app.log: written in the maintenance profile only (hmiapp/log_profile.hpp).
#ifndef AA_ENTRYNAV_HMIAPP_APP_LOG_HPP
#define AA_ENTRYNAV_HMIAPP_APP_LOG_HPP

// The build's name in the logs. No build date: unchanged code gives the same file, so an update can
// tell that the iDrive needs no restart.
#define ENTRY_AA_APP_VERSION "aa-entrynav-app-12"

namespace evo {
namespace app {

// One line: date, monotonic seconds, tag [AAEntryNavApp]. Bounded: past 512 KiB the file becomes app.old.log.
// Any thread.
void alog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace app
}  // namespace evo

#endif
