// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The ids of hmiapp/AAEntryNav.xml the app's code uses: states, events, actions, models, texts, images, components.
// The XML is parsed by the car and must not change for code reasons: these values follow it.
#ifndef AA_ENTRYNAV_HMIAPP_RHMI_IDS_HPP
#define AA_ENTRYNAV_HMIAPP_RHMI_IDS_HPP

namespace evo {
namespace app {

// ---- hmiStates -----------------------------------------------------------------------------------------------------
const unsigned kMenuState = 10;       // the app's menu
const unsigned kHelpState = 12;       // "Help", child of the menu
const unsigned kAaState = 13;         // inert blank state held while Android Auto is on screen (exclusivity)
const unsigned kFlarePinState = 14;   // the speller asking the Flare Drive PIN (inputHmiState, masked PIN mode)
const unsigned kFlarePageState = 15;  // the Flare Drive page: a text and the QR code
const unsigned kOwnPopupState = 17;   // an empty popup of the app (popupHmiState), opened by kEvOwnPopup
// State handler kinds, as bmw::PluginApp::addHandler(state, kind, ...) takes them.
const unsigned kHandlerFocus = 1;
const unsigned kHandlerVisible = 11;

// ---- focusEvents ---------------------------------------------------------------------------------------------------
const unsigned kEvGoHome = 50;         // -> NativeStateID 1 (iDrive main menu)
const unsigned kEvOpenMenu = 51;       // -> state 10 (the app's menu)
const unsigned kEvHoldAa = 52;         // -> state 13 (the blank state)
const unsigned kEvOpenPin = 53;        // -> state 14
const unsigned kEvOpenFlarePage = 54;  // -> state 15
// popupEvent -> state 17, opened (true) or closed (false). iDrive performs an app's focus event when the topmost popup
// is an app's (CFocusEvent::isCurrentlyAllowed), and a popup event is not subject to that test (CPopupEvent::perform):
// 3 popups at a time for a local app, given back when they close.
const unsigned kEvOwnPopup = 55;

// ---- raActions -----------------------------------------------------------------------------------------------------
const unsigned kActEntry = 2;  // entry button "Android Auto" (iDrive Media menu)
const unsigned kActStart = 21;
const unsigned kActStop = 22;
const unsigned kActAutostart = 24;
const unsigned kActFlareRow = 26;  // the menu's "Flare Drive" row
const unsigned kActPinOk = 27;     // the speller's OK: the whole PIN, as a string
// The speller's keystrokes. Without an inputAction the HMI never keeps the typed text, and OK sends it empty
// (CAbstractInputHmiState::spellerCurrentInputString returns at once when the state has none: seen in the car).
const unsigned kActPinInput = 28;

// ---- the entry button ----------------------------------------------------------------------------------------------
const unsigned kEntryTextModel = 3;
const unsigned kEntryImageModel = 4;
const unsigned kTextDefault = 1;     // "Android Auto"
const unsigned kTextConnecting = 2;  // "Android Auto: Connecting..."
const unsigned kTextWaiting = 3;     // "Android Auto: Plug in phone" (cable only, nothing plugged in)
const unsigned kImgEntryIcon = 55001;
const unsigned kImgSpinFirst = 55010;  // first frame of the connecting animation
const unsigned kSpinFrames = 8;

// ---- the menu ------------------------------------------------------------------------------------------------------
const unsigned kStartRowModel = 31;
const unsigned kTextStart = 10;          // "Start Android Auto"
const unsigned kTextRowConnecting = 13;  // "Connecting to the phone..."
const unsigned kTextRowOpen = 14;        // "Open Android Auto"
const unsigned kTextRowWaiting = 17;     // "Plug in the phone's USB cable"
const unsigned kAutostartModel = 34;
const unsigned kStartButton = 61;
const unsigned kStopButton = 62;  // hidden while Android Auto is stopped

// ---- Flare Drive (flare_page.hpp) ----------------------------------------------------------------------------------
const unsigned kPinResultModel = 85;    // raDataModel: the speller's text, emptied after each OK
const unsigned kPinTitleModel = 86;     // the speller's title
const unsigned kQrImageModel = 87;      // raImageModel: the QR code, as a PNG file
const unsigned kFlareStatusModel = 89;  // the page's text
const unsigned kQrImage = 72;           // the page's image component
const unsigned kTextPinChoose = 26;     // "Choose a PIN for Flare Drive (4 to 8 digits)"
const unsigned kTextPinAgain = 27;      // "Enter the new PIN again"
const unsigned kTextPinEnter = 28;      // "Enter the Flare Drive PIN"
const unsigned kTextPinWrong = 29;      // "Wrong PIN. Try again"
const unsigned kTextPinMismatch = 31;   // "The two PINs differed. Choose a PIN"
const unsigned kTextFlareScan = 32;     // "Scan this code with Flare Drive"
const unsigned kTextFlareLocked = 33;   // "Too many wrong PINs. Try again later"
const unsigned kTextFlareWifiOff = 34;  // "The car's Wi-Fi is off. Start Android Auto, then try again"
const unsigned kTextFlareDriving = 35;  // "Not available while driving"
const unsigned kTextPinInvalid = 36;    // "The PIN is 4 to 8 digits. Try again"

// Component properties, as bmw::PluginApp::setProperty takes them.
const unsigned kPropVisible = 3;
const unsigned kPropWaiting = 8;

}  // namespace app
}  // namespace evo

#endif
