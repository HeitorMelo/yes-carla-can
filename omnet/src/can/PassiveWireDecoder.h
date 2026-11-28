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
        if (invalid_) return invalidBit();
        const bool value = !dominant;

        if (!started_) {
            if (!dominant) return invalidBit();
            started_ = true;
        }

        if (stuffDue_) {
            ObservedBit result;
            result.field = previousField_;
            result.stuffed = true;
            if (value == previous_) return invalidBit(result);
            previous_ = value;
            run_ = 1;
            stuffDue_ = false;
            return result;
        }

        if (logicalCount_ >= protectedLength_) return sampleTrailer();
        return sampleProtected(value);
    }

    bool hasIdentifier() const { return logicalCount_ >= 12 && !invalid_; }
    std::uint16_t identifier() const { return id_; }

private:
    static constexpr unsigned headerBits = 19;
    static constexpr unsigned crcBits = 15;

    ObservedBit invalidBit(ObservedBit result = {}) {
        invalid_ = true;
        result.valid = false;
        return result;
    }

    ObservedBit sampleTrailer() {
        ObservedBit result;
        const unsigned position = trailingCount_++;
        if (position == 0) {
            result.field = "CRC_Delimiter";
            result.logicalIndex = 0;
        } else if (position == 1) {
            result.field = "ACK";
            result.logicalIndex = 0;
        } else if (position == 2) {
            result.field = "ACK_Delimiter";
            result.logicalIndex = 0;
        } else if (position < 10) {
            result.field = "EOF";
            result.logicalIndex = static_cast<int>(position - 3);
        } else if (position < 13) {
            result.field = "Intermission";
            result.logicalIndex = static_cast<int>(position - 10);
        } else {
            return invalidBit(result);
        }
        return result;
    }

    ObservedBit sampleProtected(bool value) {
        ObservedBit result;
        const unsigned index = logicalCount_++;
        const unsigned crcStart = headerBits + dlc_ * 8;

        if (index == 0) result.field = "SOF";
        else if (index <= 12) result.field = "Arbitration";
        else if (index < headerBits) result.field = "Control";
        else if (index < crcStart) result.field = "Data";
        else result.field = "CRC";
        result.logicalIndex = static_cast<int>(index >= crcStart ? index - crcStart : index);

        if (index >= 1 && index <= 11) {
            id_ = static_cast<std::uint16_t>((id_ << 1) | value);
        }
        if (index >= 12 && index <= 14 && value) invalid_ = true;
        if (index >= 15 && index <= 18) {
            dlc_ = (dlc_ << 1) | static_cast<unsigned>(value);
            if (index == 18) {
                if (dlc_ > 8) invalid_ = true;
                protectedLength_ = headerBits + dlc_ * 8 + crcBits;
            }
        }

        if (run_ == 0 || value != previous_) {
            previous_ = value;
            run_ = 1;
        } else {
            ++run_;
        }
        previousField_ = result.field;
        stuffDue_ = run_ == 5;
        result.valid = !invalid_;
        return result;
    }

    bool started_ = false;
    bool invalid_ = false;
    bool stuffDue_ = false;
    bool previous_ = false;
    unsigned run_ = 0;
    unsigned logicalCount_ = 0;
    unsigned protectedLength_ = headerBits + crcBits;
    unsigned dlc_ = 0;
    unsigned trailingCount_ = 0;
    std::uint16_t id_ = 0;
    const char *previousField_ = "Unknown";
};

} // namespace canlab
