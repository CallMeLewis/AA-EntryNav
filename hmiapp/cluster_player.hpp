// Copyright (c) 2026 CallMeLewis
// iDrive's CarPlay player while Android Auto's sound plays on CarPlay's audio source (0x82, audio-wifi): keeps the
// cluster from saying "no active entertainment source" and forwards cluster skips to the service.
//
// Read in HmiMain3D (202306092158, 22 November 2026):
//   - CEntModeButtonService (hmiplugin::get<IEntModeButtonService>): registered mode handlers at +0xec, nodes
//     {next, prev, handler, sequence position, player type}. CEntModeHandlerCarPlay's slot 11 (isModeHandlerAvailable)
//     answers false without an iPhone, so the cluster maps it to CDMM_NO_AUDIO. A vtable copy on that one object
//     answers true while source 0x82 is current and built up.
//   - CEntSkipService (hmiplugin::get<IEntSkipService>): map of skip devices by (audio source, waveband) at +0xa0
//     (header +0xa4, leftmost +0xac, value +0x10). CarPlay's skip device (CDevExternalDevicesSkipDevice, source 0x82)
//     gets a vtable copy whose slot 0 rebuilds the mode event when source 0x82 starts or stops, slot 4 says it plays,
//     and slot 1 sends "skip 1" / "skip -1" to the service.
// Active automatically when the audio-wifi file exists as iDrive starts; untouched otherwise. Every memory read of
// iDrive's internal tables goes through a pipe, so a bad pointer is an error, never a crash.
#ifndef AA_ENTRYNAV_HMIAPP_CLUSTER_PLAYER_HPP
#define AA_ENTRYNAV_HMIAPP_CLUSTER_PLAYER_HPP

#include <stddef.h>

namespace evo {
namespace app {

// In createApp, on HmiMain3D's thread: installs the CarPlay player and skip-device hooks when audio-wifi exists.
void captureClusterPlayer();

// From the status poll: tells the service whether the cluster shows its skip menu ("clusterskip") and what iDrive's
// entertainment source is ("carsource"). The skip state goes when it or the service's PID changes; the source when it
// changes and every 10 s, and only from a reading made for this service in the last few seconds.
void clusterPlayerPoll(int servicePid, bool connected);

// From the hmi_thread job: reads iDrive's entertainment source for the next poll. Does nothing on any thread but
// HmiMain3D's own, the only one where iDrive's per-thread plugin cache gives the audio service. The job also runs for
// the track title while no phone is connected, so readings are made then too; the poll ignores those.
void clusterPlayerSourceCheck();

// Whether the cluster player hooks are installed on iDrive's CarPlay player (set in createApp).
bool clusterPlayerInstalled();

// A read of iDrive's memory that fails instead of faulting, and the class an object's vtable names (used by
// hmi_thread.cpp to locate CHmiICProcUnit).
bool clusterPlayerPeek(const void* addr, void* out, size_t n);
void clusterPlayerClassName(const void* object, char* out, size_t size);

// In destroyApp, before the library can go: restores CarPlay's original vtables and closes the peek pipe.
void releaseClusterPlayer();

}  // namespace app
}  // namespace evo

#endif
