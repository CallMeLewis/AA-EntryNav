// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Decides when Android Auto steps aside for an iDrive popup. Pure logic, no HMI call (tests/test_popup_peek.cpp).
// Android Auto is drawn above iDrive, so anything iDrive opens over our blank state stays hidden. Such a popup takes
// the focus from that state while it stays visible, the only reliable signal we already receive without new HMI calls.
// CarPlay gets more detail from CHmiPopupDetector; that needs IHmiStateManager vtable calls this app does not make.
#ifndef AA_ENTRYNAV_HMIAPP_POPUP_PEEK_HPP
#define AA_ENTRYNAV_HMIAPP_POPUP_PEEK_HPP

class PopupPeek {
public:
    // kDriveMode: the span began within kDriveModeSeconds of a driving mode change, so it is iDrive's driving mode
    // display. iDrive only opens it when its setting to show driving mode changes is on: with the setting off, nothing
    // covers our state and Android Auto stays. The display gets its own shorter peek (the service's "drivemode"), since
    // its focus loss can outlast it (hold_machine.hpp).
    enum Action { kNone = 0, kStart = 1, kEnd = -1, kDriveMode = 2 };
    static const int kDriveModeSeconds = 6;

    PopupPeek() : peek_(false), decided_(false), shownMode_(0) {}

    // Once per status poll. covered = iDrive holds the focus over our visible state while Android Auto is on screen.
    // now: monotonic seconds. One decision per covered span: a popup still open when the time limit ends does not take
    // the picture again. A later driving mode change in the same span does: iDrive shows the new mode in the display
    // that still holds the focus, so kDriveMode again.
    Action step(bool covered, long now, long lastDriveMode) {
        if (!covered) {
            decided_ = false;
            if (!peek_) {
                return kNone;
            }
            peek_ = false;
            return kEnd;
        }
        const bool driveMode = lastDriveMode && now - lastDriveMode < kDriveModeSeconds;
        if (decided_) {
            if (!peek_ && driveMode && lastDriveMode != shownMode_) {
                shownMode_ = lastDriveMode;
                return kDriveMode;
            }
            return kNone;
        }
        decided_ = true;
        if (driveMode) {
            shownMode_ = lastDriveMode;
            return kDriveMode;
        }
        peek_ = true;
        return kStart;
    }
    // Android Auto is aside for an iDrive popup: no hold event is fired at it until it closes.
    bool active() const { return peek_; }

private:
    bool peek_, decided_;
    long shownMode_;  // the driving mode change whose display was last given a peek
};

#endif
