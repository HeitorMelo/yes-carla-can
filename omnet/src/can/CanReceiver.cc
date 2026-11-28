#include "CanCodec.h"

#include <cstddef>
#include <vector>

namespace canlab {
namespace {

constexpr std::size_t HeaderLength = 19;

unsigned readDlc(const std::vector<bool>& bits) {
    unsigned dlc = 0;
    for (unsigned index = 15; index < HeaderLength; ++index) {
        dlc = (dlc << 1) | static_cast<unsigned>(bits[index]);
    }
    return dlc;
}

} // namespace

ReceiveSampleResult CanReceiver::result(ReceiveEvent event, DecodeStatus status,
                                        std::size_t wireIndex,
                                        std::int64_t timeNs) const {
    ReceiveSampleResult value;
    value.event = event;
    value.status = status;
    value.wireBitIndex = wireIndex;
    value.timeNs = timeNs;

    if (logicalBits_.size() >= HeaderLength) {
        const unsigned dlc = readDlc(logicalBits_);
        const std::size_t frameLength = HeaderLength + dlc * 8;
        if (dlc <= 8 && logicalBits_.size() >= frameLength) {
            value.frame = decodeFrame();
        }
    }
    return value;
}

ReceiveSampleResult CanReceiver::fail(DecodeStatus status,
                                      std::size_t wireIndex,
                                      std::int64_t timeNs) {
    failed_ = true;
    phase_ = Phase::Failed;
    return result(ReceiveEvent::ErrorDetected, status, wireIndex, timeNs);
}

CanFrame CanReceiver::decodeFrame() const {
    CanFrame frame;

    for (unsigned index = 1; index <= 11; ++index) {
        frame.id = static_cast<std::uint16_t>(
            (frame.id << 1) | logicalBits_[index]);
    }
    frame.dlc = static_cast<std::uint8_t>(readDlc(logicalBits_));

    for (unsigned byte = 0; byte < frame.dlc; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const std::size_t index = HeaderLength + byte * 8 + bit;
            frame.data[byte] = static_cast<std::uint8_t>(
                (frame.data[byte] << 1) | logicalBits_[index]);
        }
    }
    return frame;
}

ReceiveSampleResult CanReceiver::finishRaw(std::size_t wireIndex,
                                          std::int64_t timeNs) {
    const std::vector<bool> protectedBits(
        logicalBits_.begin(),
        logicalBits_.begin() + static_cast<std::ptrdiff_t>(protectedLength_));
    const auto expected = CanCodec::crc15(protectedBits);

    std::uint16_t received = 0;
    for (std::size_t index = protectedLength_; index < expectedCrcBits_; ++index) {
        received = static_cast<std::uint16_t>(
            (received << 1) | logicalBits_[index]);
    }
    if (received != expected) {
        return fail(DecodeStatus::CRCError, wireIndex, timeNs);
    }

    phase_ = Phase::CRC_Delimiter;
    return result(ReceiveEvent::None, DecodeStatus::NeedMore, wireIndex, timeNs);
}

ReceiveSampleResult CanReceiver::sample(bool busDominant, std::int64_t timeNs) {
    if (phase_ == Phase::Failed) {
        return result(ReceiveEvent::None, DecodeStatus::NeedMore,
                      frameWireIndex_, timeNs);
    }

    if (phase_ == Phase::Complete) {
        if (!busDominant) {
            return result(ReceiveEvent::None, DecodeStatus::NeedMore,
                          frameWireIndex_, timeNs);
        }
        reset(); // A new dominant bit after EOF/intermission starts the next frame.
    }

    if (phase_ == Phase::Idle) {
        if (!busDominant) {
            return result(ReceiveEvent::None, DecodeStatus::NeedMore, 0, timeNs);
        }

        logicalBits_.push_back(false); // dominant SOF
        frameWireIndex_ = 1;
        runLength_ = 1;
        previousLogical_ = false;
        phase_ = Phase::RawFrame;
        return result(ReceiveEvent::None, DecodeStatus::NeedMore, 0, timeNs);
    }

    const std::size_t wireIndex = frameWireIndex_++;
    const bool logicalOne = !busDominant;

    switch (phase_) {
        case Phase::RawFrame: {
            if (expectsStuff_) {
                if (logicalOne == previousLogical_) {
                    return fail(DecodeStatus::StuffError, wireIndex, timeNs);
                }

                previousLogical_ = logicalOne;
                runLength_ = 1;
                expectsStuff_ = false;
                if (logicalBits_.size() == expectedCrcBits_) {
                    return finishRaw(wireIndex, timeNs);
                }
                return result(ReceiveEvent::None, DecodeStatus::NeedMore,
                              wireIndex, timeNs);
            }

            logicalBits_.push_back(logicalOne);
            if (logicalOne == previousLogical_) {
                ++runLength_;
            } else {
                previousLogical_ = logicalOne;
                runLength_ = 1;
            }
            if (runLength_ == 5) expectsStuff_ = true;

            if (logicalBits_.size() == 1 && logicalOne) {
                return fail(DecodeStatus::FormError, wireIndex, timeNs);
            }
            if (logicalBits_.size() == 13 && logicalOne) { // RTR is zero here.
                return fail(DecodeStatus::FormError, wireIndex, timeNs);
            }
            if (logicalBits_.size() == 14 && logicalOne) { // Standard IDE is zero.
                return fail(DecodeStatus::FormError, wireIndex, timeNs);
            }
            if (logicalBits_.size() == 15 && logicalOne) { // Reserved r0 is zero.
                return fail(DecodeStatus::FormError, wireIndex, timeNs);
            }

            if (logicalBits_.size() == HeaderLength) {
                const unsigned dlc = readDlc(logicalBits_);
                if (dlc > 8) {
                    return fail(DecodeStatus::FormError, wireIndex, timeNs);
                }
                protectedLength_ = HeaderLength + dlc * 8;
                expectedCrcBits_ = protectedLength_ + 15;
            }

            if (logicalBits_.size() == expectedCrcBits_ && !expectsStuff_) {
                return finishRaw(wireIndex, timeNs);
            }
            return result(ReceiveEvent::None, DecodeStatus::NeedMore,
                          wireIndex, timeNs);
        }

        case Phase::CRC_Delimiter:
            if (!logicalOne) {
                return fail(DecodeStatus::FormError, wireIndex, timeNs);
            }
            ackEligible_ = true;
            phase_ = Phase::ACK;
            return result(ReceiveEvent::AckEligible, DecodeStatus::Valid,
                          wireIndex, timeNs);

        case Phase::ACK:
            // The ACK slot is shared. No ACK from another node is not a receive error.
            phase_ = Phase::ACK_Delimiter;
            return result(ReceiveEvent::None, DecodeStatus::NeedMore,
                          wireIndex, timeNs);

        case Phase::ACK_Delimiter:
            if (!logicalOne) {
                return fail(DecodeStatus::FormError, wireIndex, timeNs);
            }
            phase_ = Phase::EOF_Field;
            eofBitsRemaining_ = 7;
            return result(ReceiveEvent::None, DecodeStatus::NeedMore,
                          wireIndex, timeNs);

        case Phase::EOF_Field:
            if (!logicalOne) {
                return fail(DecodeStatus::FormError, wireIndex, timeNs);
            }
            if (--eofBitsRemaining_ == 0) {
                phase_ = Phase::Complete;
                complete_ = true;
                return result(ReceiveEvent::FrameComplete, DecodeStatus::Valid,
                              wireIndex, timeNs);
            }
            return result(ReceiveEvent::None, DecodeStatus::NeedMore,
                          wireIndex, timeNs);

        case Phase::Idle:
        case Phase::Complete:
        case Phase::Failed:
            break;
    }

    return result(ReceiveEvent::None, DecodeStatus::NeedMore, wireIndex, timeNs);
}

void CanReceiver::reset() {
    phase_ = Phase::Idle;
    logicalBits_.clear();
    frameWireIndex_ = 0;
    protectedLength_ = HeaderLength;
    expectedCrcBits_ = HeaderLength + 15;
    runLength_ = 0;
    previousLogical_ = false;
    expectsStuff_ = false;
    eofBitsRemaining_ = 0;
    ackEligible_ = false;
    failed_ = false;
    complete_ = false;
}

} // namespace canlab
