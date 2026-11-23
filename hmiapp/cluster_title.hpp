// Copyright (c) 2026 CallMeLewis
// The phone's track (title, artist, album) as iDrive's CarPlay track, while audio-wifi is active
// (cluster_player.hpp): the cluster's CarPlay menu shows the title, and the main menu's Media tile and the media
// screens show title, artist and album.
//
// Both read the AuxIn proxy of CarPlay's instance, CPxyAuxiliaryInput::getServiceProxy(0x70) + 0x38c0, its
// CAuxIn_CurrentAudioFileInfo; without an iPhone CARPLAY never fills it. Read in libhmil6services.so (22 November
// 2026): a Status (CAuxIn_CurrentAudioFileInfo::onIncomingMessage) puts the position at +0x78, the field mask at +0x7a
// and the CPRAudioMediaData at +0x8c, whose texts are THmiString<unsigned short>s: title at +0x2e0 (mask byte 0 bit 1),
// artist at +0x2e4 (bit 3), album at +0x2e8 (bit 5); then setValid(true) and the delegates at +0x4c and +0x6c. The
// cluster's menu (CICCarPlaySkipController::updateTitleDetails) shows the title; the Media tile's
// CEntInfoUpdaterCarPlay (CEntInfoUpdaterAux::updateInfoText) the three, through CDevCarPlay. This does the same with
// the phone's track, nothing sent on the car's network. Each HMI thread has its own proxy (hmi_thread.hpp), so it runs
// on HmiMain3D's own thread and on the cluster's.
#ifndef AA_ENTRYNAV_HMIAPP_CLUSTER_TITLE_HPP
#define AA_ENTRYNAV_HMIAPP_CLUSTER_TITLE_HPP

namespace evo {
namespace app {

// From the status poll (any thread): reads the service's track file (contract::kTrackFile) and asks the HMI threads
// to put a changed track in place. Every few seconds it asks anyway, in case CARPLAY's own status replaced it.
void clusterTitlePoll(long nowS);

// On an HMI thread (an HmiThreadJob's work): puts the latest track in that thread's proxy when it differs from what is
// there.
void clusterTitleApply();

}  // namespace app
}  // namespace evo

#endif
