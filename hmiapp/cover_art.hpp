// Copyright (c) 2026 CallMeLewis
// The phone's album art as iDrive's CarPlay cover, while the cluster player experiment runs (ent_probe.hpp): the main
// menu's Media tile and the media screens show it.
//
// Read in HmiMain3D (202306092158, 22 November 2026). The screens that show CarPlay's track have a
// CEntInfoUpdaterCarPlay (base CEntInfoUpdaterAux), whose queryCoverArt (vtable slot 15) asks CARPLAY for the picture
// through a CCoverArtLoader; without an iPhone nothing comes. When one does, CCoverArtLoader makes a bitmap with
// CHmiBitmapManager and the updater hands it to its screen's adapter (IEntInfoAdapter slot 37,
// setAndScaleDynamicCoverArtForMM; the adapter is the updater's +0x20). Here:
//   - The picture the service saved (contract::kCoverFile) goes into a CSvcStream (libhmiservices.so; its base
//     CHmiBufferStream::readFromFile, libhmibase.so) and becomes a bitmap with
//     CHmiBitmapManager::convertCSvcStreamToBitmap, as the radio's station logos do (CRadioPictureLoader). The bitmap
//     manager is the app's own: a std::map of 0x18 bytes, as CRadioPictureLoader::init makes one.
//   - CEntInfoUpdaterCarPlay's vtable slot 15 is swapped (the vtable itself, made writable for the moment) for a
//     function that hands that bitmap to the adapter while there is one, and otherwise calls iDrive's own. Put back
//     in destroyApp.
// All of it on HmiMain3D's own thread, where the updaters live.
#ifndef AA_ENTRYNAV_HMIAPP_COVER_ART_HPP
#define AA_ENTRYNAV_HMIAPP_COVER_ART_HPP

namespace evo {
namespace app {

// In createApp, on HmiMain3D's thread: swaps the slot when everything is as read. Logs what it found.
void coverInstall();

// On HmiMain3D's thread: the picture whose hash the track file names (empty: none). True when what is shown changes,
// so that the caller has the screens ask again.
bool coverUpdate(const char* hash);

// In destroyApp: iDrive's own slot back.
void coverRelease();

}  // namespace app
}  // namespace evo

#endif
