// Copyright (c) 2026 CallMeLewis
// Which media messages of one sound stream may be acked: those whose sound has been played. No I/O, no clock, no lock.
//
// The phone may have kMaxUnacked messages of a stream unacked. Acked as they arrive, that window is never used: the
// phone sends on its own clock, the car plays on another, and all that waits between the two is the player's start-up
// delay. Acked once played, the phone keeps the window full, so about kMaxUnacked messages of sound wait in the car
// (0.4 s of music, at 43 ms a message), which a pause on the Wi-Fi drains before any is heard, and the phone can send
// no faster than the car plays. Google's Desktop Head Unit acks sound this way, and upstream (Sq00ky, 65f908e) took it
// from there.
//
// A message nobody played within the caller's limit is acked anyway: a player that takes nothing (no output, or
// stopped) slows the phone down but never stops its stream. tests/test_played_acks.cpp.
#ifndef AA_ENTRYNAV_CORE_PLAYED_ACKS_HPP
#define AA_ENTRYNAV_CORE_PLAYED_ACKS_HPP

#include <stddef.h>
#include <stdint.h>

#include <deque>

namespace aa {

// The session's limit: a message whose sound nobody played within this is acked anyway.
const uint64_t kPlayedForceMs = 1000;

class PlayedAcks {
public:
    PlayedAcks() { clear(); }

    // A new session: nothing waits, nothing counted.
    void clear() {
        waiting_.clear();
        unplayedAt_ = 0;
        late_ = 0;
        forced_ = mostWaiting_ = 0;
    }

    // A message with `bytes` of sound came, before the player can take any of it.
    void arrived(size_t bytes, uint64_t nowMs) {
        const Waiting w = {bytes, nowMs};
        waiting_.push_back(w);
        if (waiting_.size() > mostWaiting_) {
            mostWaiting_ = static_cast<unsigned>(waiting_.size());
        }
        skipPlayed();
    }

    // The player took `bytes`, oldest first, played or thrown away. True when a message is now due: the caller wakes
    // whoever sends the acks.
    bool played(size_t bytes) {
        // Sound of messages already acked for their age must not count for the ones after them.
        const size_t late = bytes < late_ ? bytes : late_;
        late_ -= late;
        bytes -= late;
        while (bytes && unplayedAt_ < waiting_.size()) {
            Waiting& w = waiting_[unplayedAt_];
            const size_t part = bytes < w.left ? bytes : w.left;
            w.left -= part;
            bytes -= part;
            skipPlayed();
        }
        // What is left belongs to no message that waits (a stream that started over): dropped.
        return due();
    }

    bool due() const { return !waiting_.empty() && waiting_.front().left == 0; }
    size_t waiting() const { return waiting_.size(); }

    // How many messages to ack now: those played to their end at the front, and those that came forceMs ago or more.
    unsigned take(uint64_t nowMs, uint64_t forceMs) {
        unsigned count = 0;
        while (!waiting_.empty()) {
            const Waiting& w = waiting_.front();
            if (w.left != 0) {
                if (nowMs - w.atMs < forceMs) {
                    break;
                }
                late_ += w.left;
                ++forced_;
            }
            waiting_.pop_front();
            if (unplayedAt_ > 0) {
                --unplayedAt_;
            }
            ++count;
        }
        return count;
    }

    // The stream starts over: every message that waits is acked now. What is still to be played of them then counts for
    // the messages that follow, which are acked a little early rather than late.
    unsigned takeAll() {
        const unsigned count = static_cast<unsigned>(waiting_.size());
        waiting_.clear();
        unplayedAt_ = 0;
        late_ = 0;
        return count;
    }

    unsigned forced() const { return forced_; }            // acked for their age, since clear()
    unsigned mostWaiting() const { return mostWaiting_; }  // the most messages unacked at once, since clear()

private:
    struct Waiting {
        size_t left;    // bytes not played yet
        uint64_t atMs;  // when it came
    };

    void skipPlayed() {
        while (unplayedAt_ < waiting_.size() && waiting_[unplayedAt_].left == 0) {
            ++unplayedAt_;
        }
    }

    std::deque<Waiting> waiting_;  // unacked, oldest first
    size_t unplayedAt_;            // the first with sound left to play, waiting_.size() when none
    size_t late_;                  // sound still to be played of messages acked for their age
    unsigned forced_, mostWaiting_;
};

}  // namespace aa

#endif  // AA_ENTRYNAV_CORE_PLAYED_ACKS_HPP
