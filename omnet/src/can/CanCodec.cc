#include "CanCodec.h"

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace canlab {
namespace {

constexpr std::uint16_t CrcPolynomial = 0x4599;
constexpr std::size_t HeaderLength = 19;
constexpr std::size_t CrcLength = 15;

struct SourceBit {
    bool value;
    Field field;
    int logicalIndex;
};

void appendWireBit(WireFrame& wire, bool logicalOne, Field field,
                   int logicalIndex, bool stuffed = false) {
    // The bus API stores dominant as true, while the frame is logical bits.
    wire.bits.push_back(Bit{!logicalOne, field, logicalIndex, stuffed});
}

void appendMostSignificantBits(std::vector<bool>& bits, std::uint32_t value,
                               unsigned width) {
    for (int bit = static_cast<int>(width) - 1; bit >= 0; --bit) {
        bits.push_back(((value >> bit) & 1u) != 0);
    }
}

void appendFieldBits(std::vector<SourceBit>& protectedBits,
                     std::vector<bool>& crcInput,
                     const std::vector<bool>& fieldBits, Field field) {
    for (bool bit : fieldBits) {
        const int index = static_cast<int>(protectedBits.size());
        protectedBits.push_back({bit, field, index});
        crcInput.push_back(bit);
    }
}

void appendStuffedBits(WireFrame& wire, const std::vector<SourceBit>& bits,
                       std::size_t crcStart) {
    if (bits.empty()) return;

    bool previous = bits.front().value;
    unsigned runLength = 1;

    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (i == crcStart) wire.crc_start = wire.bits.size();
        const auto& bit = bits[i];
        appendWireBit(wire, bit.value, bit.field, bit.logicalIndex);
        if (i == 0) continue;

        if (bit.value == previous) {
            ++runLength;
        } else {
            previous = bit.value;
            runLength = 1;
        }

        if (runLength == 5) {
            const bool stuffedValue = !previous;
            appendWireBit(wire, stuffedValue, bit.field, -1, true);
            previous = stuffedValue;
            runLength = 1;
        }
    }
}

bool readLogicalBit(const std::vector<bool>& busDominant, std::size_t index) {
    return !busDominant[index];
}

void setError(DecodeResult& result, DecodeStatus status, std::size_t wireIndex) {
    result.status = status;
    result.error_wire_index = static_cast<int>(wireIndex);
}

DecodeStatus checkCrc(const std::vector<bool>& logicalBits,
                      std::size_t protectedLength,
                      std::size_t expectedLength) {
    const std::vector<bool> protectedBits(
        logicalBits.begin(),
        logicalBits.begin() + static_cast<std::ptrdiff_t>(protectedLength));
    const auto expected = CanCodec::crc15(protectedBits);

    std::uint16_t received = 0;
    for (std::size_t i = protectedLength; i < expectedLength; ++i) {
        received = static_cast<std::uint16_t>((received << 1) | logicalBits[i]);
    }
    return received == expected ? DecodeStatus::Valid : DecodeStatus::CRCError;
}

} // namespace

std::uint16_t CanCodec::crc15(const std::vector<bool>& bits) {
    std::uint16_t crc = 0;
    for (bool bit : bits) {
        const bool feedback = ((crc >> 14) & 1u) != static_cast<unsigned>(bit);
        crc = static_cast<std::uint16_t>((crc << 1) & 0x7fff);
        if (feedback) crc ^= CrcPolynomial;
    }
    return crc;
}

WireFrame CanCodec::serialize(const CanFrame& frame) {
    frame.validate();

    WireFrame wire;
    wire.frame = frame;

    // Build SOF through DLC and payload, which are covered by the CRC.
    std::vector<SourceBit> protectedBits;
    std::vector<bool> crcInput;
    protectedBits.push_back({false, Field::SOF, 0});
    crcInput.push_back(false);

    std::vector<bool> fieldBits;
    appendMostSignificantBits(fieldBits, frame.id, 11);
    appendFieldBits(protectedBits, crcInput, fieldBits, Field::Arbitration);

    // This subset supports standard data frames: RTR, IDE and r0 are zero.
    protectedBits.push_back({false, Field::Arbitration, 12});
    protectedBits.push_back({false, Field::Control, 13});
    protectedBits.push_back({false, Field::Control, 14});
    crcInput.insert(crcInput.end(), 3, false);

    fieldBits.clear();
    appendMostSignificantBits(fieldBits, frame.dlc, 4);
    appendFieldBits(protectedBits, crcInput, fieldBits, Field::Control);

    for (unsigned byte = 0; byte < frame.dlc; ++byte) {
        fieldBits.clear();
        appendMostSignificantBits(fieldBits, frame.data[byte], 8);
        appendFieldBits(protectedBits, crcInput, fieldBits, Field::Data);
    }

    wire.unstuffed_crc_input = crcInput;
    wire.crc = crc15(crcInput);

    std::vector<SourceBit> completeBits = protectedBits;
    fieldBits.clear();
    appendMostSignificantBits(fieldBits, wire.crc, CrcLength);
    for (std::size_t i = 0; i < fieldBits.size(); ++i) {
        completeBits.push_back({fieldBits[i], Field::CRC, static_cast<int>(i)});
    }

    appendStuffedBits(wire, completeBits, protectedBits.size());
    wire.crc_end = wire.bits.size();

    appendWireBit(wire, true, Field::CRC_Delimiter, 0);
    appendWireBit(wire, true, Field::ACK, 0);
    appendWireBit(wire, true, Field::ACK_Delimiter, 0);
    for (int i = 0; i < 7; ++i) appendWireBit(wire, true, Field::EOF_Field, i);
    for (int i = 0; i < 3; ++i) appendWireBit(wire, true, Field::Intermission, i);
    return wire;
}

DecodeResult CanCodec::decodeBusBits(const std::vector<bool>& busDominant,
                                     bool requireAck, bool requireIntermission) {
    DecodeResult result;
    std::vector<bool> logicalBits;
    std::size_t wireIndex = 0;
    bool previous = false;
    unsigned runLength = 0;
    std::size_t protectedLength = HeaderLength;
    std::size_t expectedCrcLength = protectedLength + CrcLength;

    // Decode the header first to learn the payload length. Stuffing continues
    // without interruption through the CRC field.
    while (logicalBits.size() < expectedCrcLength) {
        if (wireIndex >= busDominant.size()) {
            result.status = DecodeStatus::Truncated;
            return result;
        }

        const bool logicalOne = readLogicalBit(busDominant, wireIndex++);
        logicalBits.push_back(logicalOne);
        if (runLength == 0 || logicalOne != previous) {
            previous = logicalOne;
            runLength = 1;
        } else {
            ++runLength;
        }

        if (runLength == 5) {
            if (wireIndex >= busDominant.size()) {
                result.status = DecodeStatus::Truncated;
                return result;
            }
            const bool stuffedBit = readLogicalBit(busDominant, wireIndex);
            if (stuffedBit == previous) {
                setError(result, DecodeStatus::StuffError, wireIndex);
                return result;
            }
            ++wireIndex;
            previous = stuffedBit;
            runLength = 1;
        }

        if (logicalBits.size() == HeaderLength) {
            unsigned dlc = 0;
            for (unsigned i = 15; i < HeaderLength; ++i) {
                dlc = (dlc << 1) | static_cast<unsigned>(logicalBits[i]);
            }
            if (dlc > 8) {
                setError(result, DecodeStatus::FormError, wireIndex);
                return result;
            }
            protectedLength = HeaderLength + dlc * 8;
            expectedCrcLength = protectedLength + CrcLength;
        }
    }

    if (logicalBits[0]) {
        setError(result, DecodeStatus::FormError, 0);
        return result;
    }
    if (logicalBits[12] || logicalBits[13] || logicalBits[14]) {
        setError(result, DecodeStatus::FormError, 12);
        return result;
    }

    for (unsigned i = 1; i <= 11; ++i) {
        result.frame.id = static_cast<std::uint16_t>(
            (result.frame.id << 1) | logicalBits[i]);
    }
    for (unsigned i = 15; i < HeaderLength; ++i) {
        result.frame.dlc = static_cast<std::uint8_t>(
            (result.frame.dlc << 1) | logicalBits[i]);
    }
    for (unsigned byte = 0; byte < result.frame.dlc; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const auto index = HeaderLength + byte * 8 + bit;
            result.frame.data[byte] = static_cast<std::uint8_t>(
                (result.frame.data[byte] << 1) | logicalBits[index]);
        }
    }

    if (checkCrc(logicalBits, protectedLength, expectedCrcLength) != DecodeStatus::Valid) {
        setError(result, DecodeStatus::CRCError, wireIndex);
        return result;
    }

    if (!requireAck) {
        if (busDominant.size() < wireIndex + 1) {
            result.status = DecodeStatus::Truncated;
            return result;
        }
        if (busDominant.size() > wireIndex + 1) {
            result.status = DecodeStatus::ExtraBits;
            return result;
        }
        if (busDominant[wireIndex]) {
            setError(result, DecodeStatus::FormError, wireIndex);
            return result;
        }
        result.status = DecodeStatus::Valid;
        return result;
    }

    const std::size_t tailLength = requireIntermission ? 13 : 10;
    if (busDominant.size() < wireIndex + tailLength) {
        result.status = DecodeStatus::Truncated;
        return result;
    }
    if (busDominant.size() > wireIndex + tailLength) {
        result.status = DecodeStatus::ExtraBits;
        return result;
    }
    if (busDominant[wireIndex]) {
        setError(result, DecodeStatus::FormError, wireIndex);
        return result;
    }
    ++wireIndex;
    if (!busDominant[wireIndex]) {
        setError(result, DecodeStatus::AckError, wireIndex);
        return result;
    }
    ++wireIndex;

    const auto recessiveTailEnd = requireIntermission ? busDominant.size() : wireIndex + 8;
    for (; wireIndex < recessiveTailEnd; ++wireIndex) {
        if (busDominant[wireIndex]) {
            setError(result, DecodeStatus::FormError, wireIndex);
            return result;
        }
    }
    if (requireIntermission) {
        for (; wireIndex < busDominant.size(); ++wireIndex) {
            if (busDominant[wireIndex]) {
                setError(result, DecodeStatus::FormError, wireIndex);
                return result;
            }
        }
    }

    result.status = DecodeStatus::Valid;
    return result;
}

} // namespace canlab
