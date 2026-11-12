// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The hold's decisions (hmiapp/hold_machine.hpp): which iDrive event, which control message, at which event or poll.
#include "hold_machine.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rhmi_ids.hpp"

namespace evo {
namespace app {

HoldMachine::HoldMachine(HoldIo& io)
    : io_(io),
      menuFocused_(false),
      aaVisible_(false),
      aaFocused_(false),
      holding_(false),
      hideSent_(false),
      aaSeen_(false),
      holdGaveUp_(false),
      backAsked_(false),
      prevOverlay_(false),
      holdTime_(0),
      holdShownAt_(0),
      holdTries_(0),
      menuRouteTries_(0),
      unfocusedPolls_(0),
      leftSeen_(0),
      menuHoldMs_(-1000),
      screenChangeAt_(0),
      inCall_(false),
      callEnd_(0),
      gear_(kGearUnknown),
      driveModeAt_(0),
      serviceReachable_(false) {
    memset(prevState_, 0, sizeof(prevState_));
    strcpy(prevState_, "idle");
}

void HoldMachine::say(const char* fmt, ...) {
    char line[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    io_.log(line);
}

// ---- the car --------------------------------------------------------------------------------------------------------
void HoldMachine::callChanged(bool inCall, long now) {
    if (inCall_ && !inCall) {
        callEnd_ = now;
    }
    inCall_ = inCall;
}

void HoldMachine::gearChanged(int gear) {
    gear_ = gear;
}

void HoldMachine::driveModeChanged(long now) {
    driveModeAt_ = now;
}

// ---- events 52 and 51 -----------------------------------------------------------------------------------------------
void HoldMachine::fireHold(const char* why, long now) {
    if (holdTries_ >= kMaxHoldTries) {
        say("hold: not entering state %u (%s): already tried %d times", kAaState, why, holdTries_);
        return;
    }
    ++holdTries_;
    aaSeen_ = false;
    holdTime_ = now;
    io_.triggerEvent(kEvHoldAa);
    say("hold: event %u -> state %u (%s, try %d)", kEvHoldAa, kAaState, why, holdTries_);
}

// Event 52 works from our own states (menu, Help, the blank state). Over one of iDrive's own screens it can be
// refused: Android Auto that comes up by itself (start automatically, phone reconnected) then either stays over that
// screen with the knob reaching both, or steps aside after three refusals. Both were seen on the car, and the same is
// reported on NBT Evo after an automatic start. What the driver does by hand works: our menu first (event 51), whose
// focus asks for the blank state at once (menuFocus).
void HoldMachine::holdViaMenu(const char* why, long now) {
    ++menuRouteTries_;
    aaSeen_ = false;
    holdTime_ = now;
    io_.triggerEvent(kEvOpenMenu);
    say("hold: event %u -> our menu, then state %u (%s, try %d)", kEvOpenMenu, kAaState, why, menuRouteTries_);
}

// ---- the app's own requests -----------------------------------------------------------------------------------------
// Android Auto comes to the front: iDrive goes to the blank state at once, not at the next status poll, so the knob
// never reaches the screen underneath (entry button and the menu's Open row).
void HoldMachine::showAndHold(const char* why, long now) {
    io_.send("show");
    prevOverlay_ = false;  // the coming overlay is new
    holding_ = true;
    hideSent_ = false;
    holdTries_ = 0;
    holdShownAt_ = now;
    fireHold(why, now);
}

void HoldMachine::requestShow() {
    io_.send("show");
    prevOverlay_ = false;  // the coming overlay is new: hold iDrive on the blank state
}

// ---- NAV / MEDIA / COM keys -----------------------------------------------------------------------------------------
// NAV / MEDIA / COM / steering wheel phone opened an Android Auto app (service file, written within the last 2 s):
// iDrive switched to its own navigation / media / phone screen underneath Android Auto; put it back on our blank
// state at once, so the knob does not act on that hidden screen.
bool HoldMachine::appKeyRecent() {
    long age = 0;
    // The file's age comes from its date, which the car's clock sets: a negative age is a clock set back since, not a
    // key just pressed.
    return io_.appKeyAge(&age) && age >= 0 && age <= kAppKeyS;
}

void HoldMachine::expectScreenChange(long now) {
    screenChangeAt_ = now;
}

bool HoldMachine::screenChangeExpected(long now) const {
    return screenChangeAt_ != 0 && now - screenChangeAt_ <= kAppKeyS;
}

bool HoldMachine::keepForAppKey(const char* why, long now) {
    const bool expected = screenChangeExpected(now);
    if ((!expected && !appKeyRecent()) || inCall_ || gear_ == kReverseGear) {
        return false;
    }
    const ServiceState d = io_.readService();
    if (!d.onScreen()) {
        return false;
    }
    if (expected) {
        screenChangeAt_ = 0;  // one screen change expected, one hold
        why = "iDrive changed screen for the Bluetooth audio switch";
    } else {
        io_.removeAppKey();
    }
    holdTries_ = 0;
    fireHold(why, now);
    return true;
}

// ---- iDrive's focus -------------------------------------------------------------------------------------------------
void HoldMachine::aaFocus(bool focused) {
    aaFocused_ = focused;
    if (focused) {
        // The blank state has the focus: it is shown, whatever visibility notification was or was not sent. The hold
        // worked, so its tries start over (a focus lost and given back without "visible" used them up for good, and
        // the next BACK then left the menu under Android Auto for the rest of the session).
        menuFocused_ = false;
        aaVisible_ = true;
        aaSeen_ = true;
        holdTries_ = 0;
        menuRouteTries_ = 0;
        holdGaveUp_ = false;
    } else {
        backAsked_ = false;
    }
    say("hold: state %u focus -> %d", kAaState, static_cast<int>(focused));
    if (focused && hideSent_) {
        // A screen that made Android Auto step aside (park distance control without reverse) can give the focus back
        // with no "visible" notification: that is iDrive back on the Android Auto screen too. Not after a step aside
        // of the service's own (reverse): it comes back by itself.
        backToBlank();
    }
}

// iDrive came back to our blank state by itself while Android Auto is minimized (BACK walks its history, a screen
// over it closed): bring Android Auto to the front, like CarPlay. Not in reverse or during a call; once per return,
// whether the focus or the visibility tells it first.
void HoldMachine::backToBlank() {
    if (holding_ || inCall_ || gear_ == kReverseGear || backAsked_) {
        return;
    }
    const ServiceState d = io_.readService();
    if (d.pid > 0 && !d.overlay && d.is("connected")) {
        backAsked_ = true;
        say("hold: iDrive went back to the Android Auto screen: asking the service to come to the front");
        requestShow();
    }
}

void HoldMachine::aaVisible(bool visible, long now) {
    aaVisible_ = visible;
    if (visible) {
        aaSeen_ = true;
        holdTries_ = 0;
        menuRouteTries_ = 0;
    } else {
        backAsked_ = false;
    }
    say("hold: state %u visible -> %d", kAaState, static_cast<int>(visible));
    if (!visible && keepForAppKey("key opened an Android Auto app", now)) {
        return;
    }
    if (visible) {
        backToBlank();
    }
}

void HoldMachine::menuFocus(unsigned state, bool focused, long now, long nowMs) {
    if (focused && !menuFocused_) {
        // A new BACK / tilt to our menu is the driver's own action: it gets its tries even if events 52 fired at an
        // earlier focus loss (driving mode display) used them all up without the blank state ever coming back.
        holdTries_ = 0;
    }
    menuFocused_ = focused;
    if (focused) {
        aaFocused_ = false;
    }
    say("menu state %u focus -> %d", state, static_cast<int>(focused));
    // BACK / tilt-left pop iDrive from the blank state to our menu, or a stray push opened Help under Android Auto:
    // put it straight back (the knob goes to the phone).
    if (focused && holding_ && !inCall_ && gear_ != kReverseGear) {
        const ServiceState d = io_.readService();
        if (d.onScreen() && nowMs - menuHoldMs_ >= kMenuRefireMs) {
            menuHoldMs_ = nowMs;
            fireHold("BACK/tilt popped to our menu (immediate)", now);
        }
    }
}

// ---- the status poll ------------------------------------------------------------------------------------------------
// A (re)started service knows neither the call nor the gear: started in reverse, Android Auto would come up over the
// camera. Both are sent when the service becomes reachable, and again when the phone connects.
void HoldMachine::tellService(const ServiceState& service) {
    const bool reachable = service.pid > 0;
    if ((reachable && !serviceReachable_) || (service.is("connected") && strcmp(prevState_, "connected"))) {
        io_.send(inCall_ ? "call 1" : "call 0");
        if (gear_ != kGearUnknown) {
            char msg[32];
            snprintf(msg, sizeof(msg), "gear %d", gear_);
            io_.send(msg);
        }
    }
    serviceReachable_ = reachable;
    strncpy(prevState_, service.state, sizeof(prevState_) - 1);
}

// iDrive popup over our blank state: the service steps aside so the driver sees it (hmiapp/popup_peek.hpp).
void HoldMachine::updatePopupPeek(bool covered, long now) {
    const PopupPeek::Action a = popup_.step(covered, now, driveModeAt_);
    if (a == PopupPeek::kStart) {
        io_.send("popup 1");
        say("popup: iDrive took the focus over state %u (gear %d): Android Auto picture removed", kAaState, gear_);
    } else if (a == PopupPeek::kDriveMode) {
        io_.send("drivemode");
        say("driving mode display: iDrive took the focus over state %u: Android Auto steps aside", kAaState);
    } else if (a == PopupPeek::kEnd) {
        io_.send("popup 0");
        say("popup: focus given back: Android Auto picture back");
    }
}

// Status poll (1 s). service.onScreen(): Android Auto is on screen (knob and touch go to the phone).
void HoldMachine::poll(const ServiceState& service, long now) {
    const bool overlay = service.onScreen();
    const bool rose = overlay && !prevOverlay_, fell = !overlay && prevOverlay_;
    prevOverlay_ = overlay;
    const bool blank = blankShown();
    // Covered: the blank state is still shown but none of our states has the focus (an iDrive popup over it).
    updatePopupPeek(!rose && !fell && overlay && holding_ && blank && !aaFocused_ && !inCall_ && gear_ != kReverseGear,
                    now);
    if (rose) {
        backAsked_ = false;
        holdTries_ = 0;
        menuRouteTries_ = 0;
        hideSent_ = false;
        holdGaveUp_ = false;
        holding_ = true;
        // The blank state shown but unfocused is an iDrive popup: the peek handles it, no event over it. In reverse, a
        // service that came up before it heard of the gear steps aside at once: nothing over the camera either.
        if (gear_ == kReverseGear) {
            say("hold: Android Auto came on screen in reverse: no event over the camera");
        } else if (menuFocused_) {
            fireHold("Android Auto came on screen over our menu", now);
        } else if (!aaVisible_ && inCall_) {
            fireHold("Android Auto came on screen during a call", now);
        } else if (!aaVisible_) {
            holdViaMenu("Android Auto came on screen over an iDrive screen", now);
        }
        return;
    }
    if (fell) {
        // Android Auto stepped aside by itself (phone's exit icon, signal lost) while iDrive shows our blank state:
        // give the driver the iDrive main menu. Not in reverse, not when a hard key already took iDrive elsewhere.
        if (blank && !aaFocused_ && gear_ != kReverseGear) {
            // OPTION while Android Auto is on screen (the service stepped aside, iDrive opened its option panel over
            // our blank state): the app's menu (Open / Stop / start automatically).
            io_.triggerEvent(kEvOpenMenu);
            say("hold: OPTION: event %u -> Android Auto menu", kEvOpenMenu);
        } else if (blank && !hideSent_ && gear_ != kReverseGear) {
            io_.triggerEvent(kEvGoHome);
            say("hold: Android Auto stepped aside: event %u -> iDrive main menu", kEvGoHome);
        } else {
            say("hold: Android Auto stepped aside (state %u visible %d, hard key %d, gear %d)", kAaState,
                static_cast<int>(aaVisible_), static_cast<int>(hideSent_), gear_);
        }
        holding_ = false;
        return;
    }
    if (blank) {
        callEnd_ = 0;
    }
    // Reverse gear: the camera wins, and nothing below may fire an event over it. The service hears of the gear when
    // the app does, but its state file can still say "on screen" for a poll or two, long enough for a lost focus to
    // look like one to fight; and Android Auto asked for by the Open row cannot come up until reverse ends, so its 3 s
    // wait starts again then instead of opening the menu over the camera.
    if (gear_ == kReverseGear) {
        unfocusedPolls_ = 0;
        leftSeen_ = 0;
        if (holdShownAt_) {
            holdShownAt_ = now;
        }
        return;
    }
    if (overlay && holding_ && blank && !aaFocused_ && !inCall_) {
        if (popup_.active() || (driveModeAt_ && now - driveModeAt_ < kDriveModeQuietS)) {
            return;  // an iDrive popup or the driving mode display: Android Auto steps aside, no event over it
        }
        if (++unfocusedPolls_ >= 2 && now - holdTime_ >= 1) {
            unfocusedPolls_ = 0;
            fireHold("state lost focus", now);
        }
        return;
    }
    unfocusedPolls_ = 0;
    // Event 52 fired for the Open row or the entry button, but Android Auto never came up (session dropped meanwhile):
    // give the driver the menu back instead of a blank screen.
    if (!overlay && holding_ && blank && holdShownAt_ && now - holdShownAt_ >= 3) {
        holdShownAt_ = 0;
        holding_ = false;
        io_.triggerEvent(kEvOpenMenu);
        say("hold: Android Auto did not come up after the blank state: event %u -> menu", kEvOpenMenu);
        return;
    }
    if (overlay) {
        holdShownAt_ = 0;
    }
    if (!overlay || !holding_ || blank) {
        if (blank) {
            leftSeen_ = 0;  // iDrive came back: a later departure needs its own two polls
        }
        return;
    }
    if (now - holdTime_ < 1) {
        return;
    }
    if (menuFocused_) {
        fireHold("BACK returned to our menu", now);
        return;
    }
    if (inCall_) {
        aaSeen_ = true;
        return;
    }
    if (callEnd_ && now - callEnd_ < kCallEndS) {
        callEnd_ = 0;
        fireHold("call ended", now);
        return;
    }
    if (!aaSeen_) {
        // Still on an iDrive screen a second after asking: through our menu (holdViaMenu), then event 52 once if the
        // menu never opened.
        if (menuRouteTries_ < kMenuRouteTries) {
            holdViaMenu("state not shown over an iDrive screen", now);
        } else if (holdTries_ == 0) {
            fireHold("our menu did not open: state directly", now);
        } else if (!holdGaveUp_) {
            // Never leave Android Auto over an iDrive screen that would get the knob too: it steps aside, and the
            // Android Auto entry brings it back.
            holdGaveUp_ = true;
            hideSent_ = true;
            holding_ = false;
            io_.send("hide");
            say("hold: iDrive did not show state %u, even through our menu: Android Auto steps aside", kAaState);
        }
        return;
    }
    // A hard key (MENU / MEDIA / NAV / COM / MAP / OPTION) took iDrive elsewhere, the way MENU leaves native CarPlay:
    // Android Auto steps aside; the Android Auto entry brings it back. Confirmed on a second poll.
    if (keepForAppKey("key opened an Android Auto app (poll)", now)) {
        return;
    }
    if (!leftSeen_ || now - leftSeen_ > 3) {
        leftSeen_ = now;
        return;
    }
    leftSeen_ = 0;
    hideSent_ = true;
    holding_ = false;
    io_.send("hide");
    say("hold: iDrive left state %u (hard key): asked the service to step aside", kAaState);
}

}  // namespace app
}  // namespace evo
