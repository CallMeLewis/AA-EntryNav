// Copyright (c) 2026 CallMeLewis
// A temporary, read-only look at iDrive's media players and skip devices, while its file exists (diagnostic only).
//
// The cluster says "no active entertainment source" while Android Auto's sound plays on CarPlay's route: no player
// (mode handler) of iDrive's plays that source. Before the app registers anything of its own, this logs what iDrive
// has, as iDrive itself holds it (HmiMain3D, 202306092158, static analysis of 22 November 2026):
//   - CEntModeButtonService (hmiplugin::get<IEntModeButtonService>): its list of registered mode handlers at +0xec,
//     nodes {next, prev, handler, sequence position, player type}, and the current node at +0xe8. What the current
//     handler's player type is decides the cluster's audio mode (CICEntertainmentSourcesListController).
//   - CEntSkipService (hmiplugin::get<IEntSkipService>): its current skip device at +0x40, and its map of skip devices
//     by (audio source, waveband) at +0xa0 (a std::map: header at +0xa4, leftmost node at +0xac, value at node +0x10).
// Both services are taken in createApp, on HmiMain3D's own thread (their plugin cache is per thread), and only when
// their vtable is the one read. Every read of iDrive's memory goes through a pipe, so a bad pointer is an error, never
// a crash. Nothing is written, called or registered. Remove it once the player is built (remove-temporary-logging).
#ifndef AA_ENTRYNAV_HMIAPP_ENT_PROBE_HPP
#define AA_ENTRYNAV_HMIAPP_ENT_PROBE_HPP

#include <stddef.h>

namespace evo {
namespace app {

// In createApp, on HmiMain3D's thread: finds both services when the probe file exists, and logs what they hold.
void captureEntProbe();

// From the status poll: logs again when the current player, the current skip device or the counts change, and at most
// once a minute while unchanged with a session connected. connected: the service says a phone is connected.
void entProbePoll(bool connected, long nowS);

// Whether the cluster player experiment is in place on iDrive's CarPlay player (set in createApp).
bool clusterPlayerInstalled();

// A read of iDrive's memory that fails instead of faulting, and the class an object's vtable names (diagnostics).
bool entProbePeek(const void* addr, void* out, size_t n);
void entProbeClassName(const void* object, char* out, size_t size);

// In destroyApp, before the library can go: CarPlay's player gets its own vtable back (the cluster player experiment,
// ent_probe.cpp), since the copy and its function live in this library.
void releaseEntProbe();

}  // namespace app
}  // namespace evo

#endif
