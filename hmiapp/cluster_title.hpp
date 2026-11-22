// Copyright (c) 2026 CallMeLewis
// The phone's track title on the cluster's CarPlay menu, while the cluster player experiment runs (ent_probe.hpp).
//
// The menu (CICCarPlaySkipController, from CICUSBSkipController::updateTitleDetails) shows one line: the title in
// the AuxIn proxy of CarPlay's instance, CPxyAuxiliaryInput::getServiceProxy(0x70) + 0x38c0, its
// CAuxIn_CurrentAudioFileInfo. Without an iPhone CARPLAY never fills it. Read in libhmil6services.so (22 November
// 2026): a Status (CAuxIn_CurrentAudioFileInfo::onIncomingMessage) puts the position at +0x78, the field mask at +0x7a
// and the CPRAudioMediaData at +0x8c, whose title (mask byte 0 bit 1) is the THmiString<unsigned short> at +0x2e0; then
// setValid(true) and the delegates at +0x4c and +0x6c, which the menu and iDrive's CarPlay player listen to. This does
// the same with the phone's title, on the cluster's HMI thread, whose proxies the menu reads (hmi_thread.hpp), nothing
// sent on the car's network.
#ifndef AA_ENTRYNAV_HMIAPP_CLUSTER_TITLE_HPP
#define AA_ENTRYNAV_HMIAPP_CLUSTER_TITLE_HPP

namespace evo {
namespace app {

// From the status poll (any thread): reads the service's track file (contract::kTrackFile) and asks HmiMain3D's
// thread to put a changed title in place. Every few seconds it asks anyway, in case CARPLAY's own status replaced it.
void clusterTitlePoll(long nowS);

// On the cluster's HMI thread (an HmiThreadJob's work): puts the latest title in the proxy when it differs from what is
// there.
void clusterTitleApply();

}  // namespace app
}  // namespace evo

#endif
