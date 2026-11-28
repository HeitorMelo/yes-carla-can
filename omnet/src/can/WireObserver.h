#pragma once

#include <cstdint>

namespace canlab {

// Passive SOF/header destuffer. Input consists only of resolved bus samples;
// idle() is called after an error sequence or the completed intermission.
class WireObserver {
public:
    void idle() { *this = WireObserver{}; }

    void sample(bool dominant) {
        const bool bit = !dominant;
        if (!started_) {
            if (!dominant) return;
            started_ = true;
            logicalCount_ = 1; // SOF
            last_ = false;
            run_ = 1;
            return;
        }
        if (invalid_) return;

        if (stuffDue_) {
            if (bit == last_) {
                invalid_ = true;
                return;
            }
            last_ = bit;
            run_ = 1;
            stuffDue_ = false;
            return;
        }

        if (logicalCount_ >= 1 && logicalCount_ <= 11) {
            id_ = static_cast<std::uint16_t>((id_ << 1) | bit);
        }
        if (logicalCount_ >= 12 && logicalCount_ <= 14 && bit) {
            invalid_ = true; // unsupported RTR, IDE, r0
        }
        if (logicalCount_ >= 15 && logicalCount_ <= 18) {
            dlc_ = (dlc_ << 1) | static_cast<unsigned>(bit);
        }
        ++logicalCount_;

        if (bit == last_) {
            ++run_;
        } else {
            last_ = bit;
            run_ = 1;
        }
        if (run_ == 5) stuffDue_ = true;
    }

    bool nextDataBitIs(std::uint16_t id, int logicalIndex) const {
        return started_ && !invalid_ && !stuffDue_ && logicalCount_ >= 19 &&
            dlc_ <= 8 && logicalIndex >= 19 &&
            static_cast<unsigned>(logicalIndex) < 19 + dlc_ * 8 &&
            id_ == id && logicalCount_ == static_cast<unsigned>(logicalIndex);
    }

private:
    bool started_ = false;
    bool invalid_ = false;
    bool last_ = false;
    bool stuffDue_ = false;
    unsigned logicalCount_ = 0;
    unsigned run_ = 0;
    unsigned dlc_ = 0;
    std::uint16_t id_ = 0;
};

} // namespace canlab
