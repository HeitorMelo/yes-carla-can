#include "CanCodec.h"
#include <stdexcept>

namespace canlab {
namespace {
constexpr std::uint16_t CRC_POLY = 0x4599;

void append(WireFrame& out, bool logicalOne, Field field, int logicalIndex, bool stuffed = false) {
    // The serialized source is represented as logical zero/one. Convert it to
    // the electrical abstraction used by the bus API: dominant=true for zero.
    out.bits.push_back(Bit{!logicalOne, field, logicalIndex, stuffed});
}

struct SourceBit { bool value; Field field; int logicalIndex; };

void appendStuffed(WireFrame& out, const std::vector<SourceBit>& source) {
    if (source.empty()) return;
    bool last = source.front().value;
    unsigned run = 1;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const bool value = source[i].value;
        append(out, value, source[i].field, source[i].logicalIndex);
        if (i == 0) continue;
        if (value == last) ++run;
        else { last = value; run = 1; }
        if (run == 5) {
            const bool stuffedValue = !last;
            append(out, stuffedValue, source[i].field, -1, true);
            last = stuffedValue;
            run = 1;
        }
    }
}

void appendMsb(std::vector<bool>& bits, std::uint32_t value, unsigned width) {
    for (int i = static_cast<int>(width) - 1; i >= 0; --i)
        bits.push_back(((value >> i) & 1u) != 0);
}
} // namespace

std::uint16_t CanCodec::crc15(const std::vector<bool>& bits) {
    std::uint16_t crc = 0;
    for (bool bit : bits) {
        const bool feedback = ((crc >> 14) & 1u) != static_cast<unsigned>(bit);
        crc = static_cast<std::uint16_t>((crc << 1) & 0x7fff);
        if (feedback) crc ^= CRC_POLY;
    }
    return crc;
}

WireFrame CanCodec::serialize(const CanFrame& frame) {
    frame.validate();
    WireFrame out;
    out.frame = frame;

    // SOF through DLC are CRC input. Standard format: SOF, ID[10:0], RTR, IDE, r0, DLC.
    std::vector<SourceBit> protectedBits;
    protectedBits.push_back({false, Field::SOF, 0});
    std::vector<bool> crcInput{false};
    std::vector<bool> fieldBits;
    appendMsb(fieldBits, frame.id, 11);
    for (bool b : fieldBits) { protectedBits.push_back({b, Field::Arbitration, static_cast<int>(protectedBits.size())}); crcInput.push_back(b); }
    protectedBits.push_back({false, Field::Arbitration, 12}); crcInput.push_back(false); // RTR
    protectedBits.push_back({false, Field::Control, 13}); crcInput.push_back(false); // IDE
    protectedBits.push_back({false, Field::Control, 14}); crcInput.push_back(false); // r0
    fieldBits.clear(); appendMsb(fieldBits, frame.dlc, 4);
    for (bool b : fieldBits) { protectedBits.push_back({b, Field::Control, static_cast<int>(protectedBits.size())}); crcInput.push_back(b); }
    for (unsigned byte = 0; byte < frame.dlc; ++byte) {
        fieldBits.clear(); appendMsb(fieldBits, frame.data[byte], 8);
        for (bool b : fieldBits) { protectedBits.push_back({b, Field::Data, static_cast<int>(protectedBits.size())}); crcInput.push_back(b); }
    }

    out.unstuffed_crc_input = crcInput;
    const auto crc = crc15(crcInput);
    out.crc = crc;
    std::vector<SourceBit> complete = protectedBits;
    fieldBits.clear(); appendMsb(fieldBits, crc, 15);
    // Compute the wire offset by counting stuff insertions in the protected prefix.
    out.crc_start = 0;
    bool prev = false; unsigned run = 0;
    for (const auto& b : protectedBits) {
        if (run == 0) { prev = b.value; run = 1; }
        else if (b.value == prev) ++run;
        else { prev = b.value; run = 1; }
        ++out.crc_start;
        if (run == 5) { ++out.crc_start; prev = !prev; run = 1; }
    }
    for (std::size_t i = 0; i < fieldBits.size(); ++i) complete.push_back({fieldBits[i], Field::CRC, static_cast<int>(i)});
    appendStuffed(out, complete);
    out.crc_end = out.bits.size();

    append(out, true, Field::CRC_Delimiter, 0);
    append(out, true, Field::ACK, 0); // recessive transmitter slot; receivers may pull dominant
    append(out, true, Field::ACK_Delimiter, 0);
    for (int i = 0; i < 7; ++i) append(out, true, Field::EOF_Field, i);
    for (int i = 0; i < 3; ++i) append(out, true, Field::Intermission, i);
    return out;
}

DecodeResult CanCodec::decodeBusBits(const std::vector<bool>& busDominant, bool requireAck,
                                     bool requireIntermission) {
    DecodeResult result;
    std::vector<bool> logical;
    std::size_t wireIndex = 0;
    bool previous = false;
    unsigned run = 0;

    // Decode through the DLC first, then extend the expected unstuffed length
    // using the payload size. Stuffing applies continuously through CRC.
    std::size_t crcProtectedLength = 19; // SOF + arbitration/control through DLC
    std::size_t requiredCrcBits = crcProtectedLength + 15;
    while (logical.size() < requiredCrcBits) {
        if (wireIndex >= busDominant.size()) {
            result.status = DecodeStatus::Truncated;
            return result;
        }
        const bool value = !busDominant[wireIndex]; // recover logical recessive=1
        logical.push_back(value);
        ++wireIndex;
        if (run == 0) { previous = value; run = 1; }
        else if (value == previous) ++run;
        else { previous = value; run = 1; }

        if (run == 5) {
            if (wireIndex >= busDominant.size()) {
                result.status = DecodeStatus::Truncated;
                return result;
            }
            const bool stuffedLogical = !busDominant[wireIndex];
            if (stuffedLogical == previous) {
                result.status = DecodeStatus::StuffError;
                result.error_wire_index = static_cast<int>(wireIndex);
                return result;
            }
            ++wireIndex; // consume and exclude the stuff bit
            previous = stuffedLogical;
            run = 1;
        }

        if (logical.size() == 19) {
            unsigned dlc = 0;
            for (unsigned i = 15; i < 19; ++i) dlc = (dlc << 1) | static_cast<unsigned>(logical[i]);
            if (dlc > 8) {
                result.status = DecodeStatus::FormError;
                result.error_wire_index = static_cast<int>(wireIndex);
                return result;
            }
            crcProtectedLength = 19 + dlc * 8;
            requiredCrcBits = crcProtectedLength + 15;
        }
    }

    if (logical[0]) {
        result.status = DecodeStatus::FormError;
        result.error_wire_index = 0;
        return result;
    }
    // Standard data format has RTR=IDE=r0=0.
    if (logical[12] || logical[13] || logical[14]) {
        result.status = DecodeStatus::FormError;
        result.error_wire_index = 12;
        return result;
    }

    std::uint16_t id = 0;
    for (unsigned i = 1; i <= 11; ++i) id = static_cast<std::uint16_t>((id << 1) | logical[i]);
    result.frame.id = id;
    result.frame.dlc = 0;
    for (unsigned i = 15; i < 19; ++i)
        result.frame.dlc = static_cast<std::uint8_t>((result.frame.dlc << 1) | logical[i]);
    for (unsigned byte = 0; byte < result.frame.dlc; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit)
            result.frame.data[byte] = static_cast<std::uint8_t>((result.frame.data[byte] << 1) | logical[19 + byte * 8 + bit]);
    }

    std::vector<bool> crcInput(logical.begin(), logical.begin() + static_cast<std::ptrdiff_t>(crcProtectedLength));
    const auto expectedCrc = crc15(crcInput);
    std::uint16_t receivedCrc = 0;
    for (std::size_t i = crcProtectedLength; i < requiredCrcBits; ++i)
        receivedCrc = static_cast<std::uint16_t>((receivedCrc << 1) | logical[i]);
    if (receivedCrc != expectedCrc) {
        result.status = DecodeStatus::CRCError;
        result.error_wire_index = static_cast<int>(wireIndex);
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
            result.status = DecodeStatus::FormError;
            result.error_wire_index = static_cast<int>(wireIndex);
            return result;
        }
        result.status = DecodeStatus::Valid;
        return result;
    }

    const std::size_t tailLength = requireIntermission ? 13 : 10; // CRC delimiter, ACK, ACK delimiter, EOF, optional intermission
    if (busDominant.size() < wireIndex + tailLength) {
        result.status = DecodeStatus::Truncated;
        return result;
    }
    if (busDominant.size() > wireIndex + tailLength) {
        result.status = DecodeStatus::ExtraBits;
        return result;
    }
    // CRC delimiter, ACK, ACK delimiter, EOF, and intermission.
    if (busDominant[wireIndex]) {
        result.status = DecodeStatus::FormError;
        result.error_wire_index = static_cast<int>(wireIndex);
        return result;
    }
    ++wireIndex;
    if (!busDominant[wireIndex]) {
        result.status = DecodeStatus::AckError;
        result.error_wire_index = static_cast<int>(wireIndex);
        return result;
    }
    ++wireIndex;
    // The ACK delimiter and EOF are recessive; intermission is optional when
    // delivery is committed at the end of EOF instead of bus-idle time.
    const auto recessiveTailEnd = requireIntermission ? busDominant.size() : wireIndex + 8;
    for (; wireIndex < recessiveTailEnd; ++wireIndex) {
        if (busDominant[wireIndex]) {
            result.status = DecodeStatus::FormError;
            result.error_wire_index = static_cast<int>(wireIndex);
            return result;
        }
    }
    if (requireIntermission) {
        for (; wireIndex < busDominant.size(); ++wireIndex) {
            if (busDominant[wireIndex]) {
                result.status = DecodeStatus::FormError;
                result.error_wire_index = static_cast<int>(wireIndex);
                return result;
            }
        }
    }
    result.status = DecodeStatus::Valid;
    return result;
}

} // namespace canlab
