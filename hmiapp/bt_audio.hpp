// Copyright (c) 2026 CallMeLewis
// iDrive's own source switch, sent from the app: the "mode player select" event that iDrive's Media source popup and
// MODE button send, with Bluetooth audio's entry, to HmiMain3D's event queue. iDrive's entertainment code then does
// what it does when the owner picks Bluetooth audio: the source becomes Bluetooth audio (or waits for the phone's
// audio link if it is not up yet), and with iDrive's media app in front it also opens its Bluetooth audio screen,
// which the hold machine expects (expectScreenChange). Reverse-engineered on the copied rootfs, 202306092158:
//   - CEntMediaInstanceService::setAudioSourceByPlayerType (0x02742110) is createEvent(0), activatePlayer(focus id),
//     getHmiProcUnit()->sendEvent(event); player type 13 ("BLUE") maps to focus id 2270 (table 0x38a7078 + 314 + 2t),
//     the entry AAEntryNav.xml inserts the Android Auto row after.
//   - getHmiProcUnit() answers on HmiMain3D's own threads only (thread-local cache): the app's posted tasks run on
//     bmw::Dispatcher threads, so the queue is taken in createApp, which HmiMain3D calls on its own thread.
//   - CHmiProcUnit's vtable slot 5 is sendEvent (libhmicom.so); it queues the event under a lock, from any thread.
#ifndef AA_ENTRYNAV_HMIAPP_BT_AUDIO_HPP
#define AA_ENTRYNAV_HMIAPP_BT_AUDIO_HPP

namespace evo {
namespace app {

// In createApp, on HmiMain3D's thread: keeps the HMI event queue for later. Logs what it found.
void captureHmiQueue();

// Sends the switch to Bluetooth audio. False, with why, when the queue or one of iDrive's functions is missing or not
// as expected; nothing is sent then. From the app thread.
bool sendBluetoothAudioSelect(const char** why);

}  // namespace app
}  // namespace evo

#endif
