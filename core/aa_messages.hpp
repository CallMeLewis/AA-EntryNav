// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The messages the head unit sends to the phone, as bytes: the 2-byte id, then the body. No state, no I/O.
#ifndef AA_ENTRYNAV_CORE_AA_MESSAGES_HPP
#define AA_ENTRYNAV_CORE_AA_MESSAGES_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "aa_input.hpp"

namespace aa {

typedef std::vector<uint8_t> Message;

// Message id of a received message; 0 (media data) when it is shorter than an id, as the dispatch has always read it.
uint16_t messageId(const Message& message);
// A varint field of a received message's body, cut to 32 bits; kNoField when missing or unreadable.
const uint32_t kNoField = 0xFFFFFFFFu;
uint32_t bodyField(const Message& message, uint32_t number);
// A string field of a received message's body, at most `max` bytes of printable ASCII; empty when missing, longer or
// anything else. Enough for what is only logged or handed on, such as a Bluetooth address.
std::string bodyText(const Message& message, uint32_t number, size_t max);

namespace message {

// Control channel.
Message versionRequest();
Message tlsHandshake(const std::vector<uint8_t>& records);
Message authComplete();
Message discoveryResponse(const std::vector<uint8_t>& description);
Message channelOpenResponse();
// Echoes the request's timestamp bytes as they came, when the request starts with that field.
Message pingResponse(const Message& request);
// The head unit's own ping, carrying its clock. The phone answers it with a ping response.
Message pingRequest(uint64_t timestampUs);
Message audioFocusResponse(uint32_t state);
Message navigationFocusResponse();
Message byeByeResponse();

// Audio, video and microphone channels.
Message setupResponse();
Message videoFocusIndication(bool projected, bool unrequested);
Message mediaAck(uint32_t session);
Message micOpenResponse();
Message micData(uint64_t timestampUs, const std::vector<uint8_t>& pcm);

// Sensor channel (the location messages are in core/car_location.hpp).
Message sensorStartResponse(bool ok);
Message nightModeEvent(bool night);
Message drivingStatusEvent(uint32_t status);

// Input channel.
Message bindingResponse();
Message keyEvent(uint64_t timestampUs, uint32_t keycode, bool down);
Message rotaryEvent(uint64_t timestampUs, uint32_t keycode, int delta);
Message touchEvent(uint64_t timestampUs, const TouchEventOut& touch);

// Bluetooth channel: the answer to the phone's pairing request, always a success. Not paired: the phone pairs
// (the session only ever answers already paired: aa_channels.cpp, onBluetooth).
Message pairingResponse(bool alreadyPaired);

}  // namespace message
}  // namespace aa

#endif  // AA_ENTRYNAV_CORE_AA_MESSAGES_HPP
