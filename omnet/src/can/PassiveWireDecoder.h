#pragma once

#include <cstdint>

namespace canlab {

// Streaming, read-only classification of already sampled standard CAN bits.
// The caller supplies no frame ID, payload, or intended future position.
struct ObservedBit {
    const char *field = "Unknown";
    int logicalIndex = -1;
    bool stuffed = false;
    bool valid = true;
};

class PassiveWireDecoder {
public:
    void reset() { *this = PassiveWireDecoder{}; }

    ObservedBit sample(bool dominant) {
        ObservedBit result;
        if (invalid_) { result.valid = false; return result; }
        const bool value = !dominant;
        if (!started_) {
            if (!dominant) { invalid_ = true; result.valid = false; return result; }
            started_ = true;
        }
        if (stuffDue_) {
            result.field = previousField_;
            result.stuffed = true;
            if (value == previous_) { invalid_ = true; result.valid = false; return result; }
            previous_ = value; run_ = 1; stuffDue_ = false;
            return result;
        }
        if (logicalCount_ >= protectedLength_) {
            const unsigned pos = trailingCount_++;
            if (pos == 0) { result.field = "CRC_Delimiter"; result.logicalIndex = 0; }
            else if (pos == 1) { result.field = "ACK"; result.logicalIndex = 0; }
            else if (pos == 2) { result.field = "ACK_Delimiter"; result.logicalIndex = 0; }
            else if (pos < 10) { result.field = "EOF"; result.logicalIndex = static_cast<int>(pos - 3); }
            else if (pos < 13) { result.field = "Intermission"; result.logicalIndex = static_cast<int>(pos - 10); }
            else { invalid_ = true; result.valid = false; }
            return result;
        }
        const unsigned index = logicalCount_++;
        if (index == 0) result.field = "SOF";
        else if (index <= 12) result.field = "Arbitration";
        else if (index < 19) result.field = "Control";
        else if (index < 19 + dlc_ * 8) result.field = "Data";
        else result.field = "CRC";
        result.logicalIndex = index >= 19 + dlc_ * 8 ? static_cast<int>(index - (19 + dlc_ * 8)) : static_cast<int>(index);
        if (index < 19) result.logicalIndex = static_cast<int>(index);
        if (index >= 1 && index <= 11) id_ = static_cast<std::uint16_t>((id_ << 1) | value);
        if ((index == 12 || index == 13 || index == 14) && value) invalid_ = true;
        if (index >= 15 && index <= 18) {
            dlc_ = (dlc_ << 1) | static_cast<unsigned>(value);
            if (index == 18) {
                if (dlc_ > 8) invalid_ = true;
                protectedLength_ = 19 + dlc_ * 8 + 15;
            }
        }
        if (run_ == 0) { previous_ = value; run_ = 1; }
        else if (value == previous_) ++run_;
        else { previous_ = value; run_ = 1; }
        previousField_ = result.field;
        if (run_ == 5) stuffDue_ = true;
        result.valid = !invalid_;
        return result;
    }

    bool hasIdentifier() const { return logicalCount_ >= 12 && !invalid_; }
    std::uint16_t identifier() const { return id_; }

private:
    bool started_ = false, invalid_ = false, stuffDue_ = false, previous_ = false;
    unsigned run_ = 0, logicalCount_ = 0, protectedLength_ = 34, dlc_ = 0, trailingCount_ = 0;
    std::uint16_t id_ = 0;
    const char *previousField_ = "Unknown";
};

} // namespace canlab
