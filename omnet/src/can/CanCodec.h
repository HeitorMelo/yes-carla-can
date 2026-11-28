#pragma once

#include "CanFrame.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace canlab {

enum class Field { SOF, Arbitration, Control, Data, CRC, CRC_Delimiter, ACK, ACK_Delimiter, EOF_Field, Intermission };

struct Bit {
    // CAN dominant is logical zero; recessive is logical one.
    bool dominant = false;
    Field field = Field::SOF;
    int logical_bit_index = -1;
    bool stuffed = false;
};

struct WireFrame {
    CanFrame frame;
    std::vector<bool> unstuffed_crc_input;
    std::vector<Bit> bits;
    std::uint16_t crc = 0;
    std::size_t crc_start = 0;
    std::size_t crc_end = 0; // exclusive
};

enum class DecodeStatus { NeedMore, Valid, StuffError, CRCError, FormError, AckError, Truncated, ExtraBits };

enum class ReceiveEvent { None, AckEligible, ErrorDetected, FrameComplete };

struct DecodeResult {
    DecodeStatus status = DecodeStatus::Truncated;
    CanFrame frame{};
    // Index into the sampled wire vector, or -1 when no single bad bit applies.
    int error_wire_index = -1;
    explicit operator bool() const { return status == DecodeStatus::Valid; }
};

struct ReceiveSampleResult {
    ReceiveEvent event = ReceiveEvent::None;
    DecodeStatus status = DecodeStatus::NeedMore;
    CanFrame frame{};
    std::size_t wireBitIndex = 0;
    std::int64_t timeNs = 0;
};

// Per-controller incremental receive checker. Call once per resolved bus bit.
// On ErrorDetected it latches until reset(), allowing the wrapper to model the
// controller's error flag and reset at the end of that error sequence.
class CanReceiver {
public:
    ReceiveSampleResult sample(bool busDominant, std::int64_t timeNs);
    void reset();
    bool ackEligible() const { return ackEligible_; }
    bool failed() const { return failed_; }
    bool complete() const { return complete_; }

private:
    enum class Phase { Idle, RawFrame, CRC_Delimiter, ACK, ACK_Delimiter, EOF_Field, Complete, Failed };
    ReceiveSampleResult result(ReceiveEvent event, DecodeStatus status, std::size_t wireIndex,
                               std::int64_t timeNs) const;
    ReceiveSampleResult fail(DecodeStatus status, std::size_t wireIndex, std::int64_t timeNs);
    ReceiveSampleResult finishRaw(std::size_t wireIndex, std::int64_t timeNs);
    CanFrame decodeFrame() const;

    Phase phase_ = Phase::Idle;
    std::vector<bool> logicalBits_;
    std::size_t frameWireIndex_ = 0;
    std::size_t protectedLength_ = 19;
    std::size_t expectedCrcBits_ = 34;
    unsigned runLength_ = 0;
    bool previousLogical_ = false;
    bool expectsStuff_ = false;
    unsigned eofBitsRemaining_ = 0;
    bool ackEligible_ = false;
    bool failed_ = false;
    bool complete_ = false;
};

class CanCodec {
public:
    static std::uint16_t crc15(const std::vector<bool>& bits);
    static WireFrame serialize(const CanFrame& frame);
    // Input is resolved bus samples; true means dominant. With requireAck=true
    // supply through EOF (and intermission by default). With false supply only
    // through CRC delimiter to decide ACK eligibility before the ACK slot.
    // Set requireIntermission=false to validate/deliver at EOF.
    static DecodeResult decodeBusBits(const std::vector<bool>& busDominant, bool requireAck = true,
                                      bool requireIntermission = true);
};

} // namespace canlab
