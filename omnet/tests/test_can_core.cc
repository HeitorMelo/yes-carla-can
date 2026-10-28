#include "../src/can/CanBus.h"
#include "../src/can/CanController.h"
#include "../src/can/PassiveWireDecoder.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace canlab;

int main() {
    // Controller-level event fixtures exercise state boundaries without a bus driver.
    std::ifstream fixture("tests/controller_rule_fixture.csv");
    std::ofstream fixtureTrace("build/controller-fixture-events.jsonl");
    assert(fixture && fixtureTrace);
    std::map<std::string, CanController> fixtureControllers;
    std::string line;
    unsigned fixtureStep = 0;
    while (std::getline(fixture, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::stringstream input(line);
        std::string name, action, countStr, tecStr, recStr, expectedState, mayTxStr;
        assert(std::getline(input, name, ',') && std::getline(input, action, ',') &&
               std::getline(input, countStr, ',') && std::getline(input, tecStr, ',') &&
               std::getline(input, recStr, ',') && std::getline(input, expectedState, ',') &&
               std::getline(input, mayTxStr, ','));
        auto& ctl = fixtureControllers[name];
        for (int i = 0; i < std::stoi(countStr); ++i) {
            const auto beforeTec = ctl.tec(), beforeRec = ctl.rec();
            if (action == "bit_error") ctl.transmitError(ErrorKind::Bit);
            else if (action == "ack_error") ctl.transmitError(ErrorKind::Ack);
            else if (action == "tx_success") ctl.successfulTransmit();
            else if (action == "receive_error") ctl.receiveError(ErrorKind::CRC);
            else if (action == "rx_success") ctl.successfulReceive();
            else if (action == "blocked_request")
                assert(!ctl.mayTransmit() && !ctl.mayAcknowledge() && !ctl.emitsActiveErrorFlag());
            else assert(false && "unknown controller fixture operation");
            const char *traceState = ctl.state() == ControllerState::BusOff ? "bus_off" :
                                     ctl.state() == ControllerState::ErrorPassive ? "error_passive" : "error_active";
            fixtureTrace << "{\"step\":" << fixtureStep++ << ",\"controller\":\"" << name
                         << "\",\"event\":\"" << action << "\",\"tec_before\":" << beforeTec
                         << ",\"tec_after\":" << ctl.tec() << ",\"rec_before\":" << beforeRec
                         << ",\"rec_after\":" << ctl.rec() << ",\"state\":\"" << traceState
                         << "\",\"may_transmit\":"
                         << (ctl.mayTransmit() ? "true" : "false") << "}\n";
        }
        const auto state = ctl.state() == ControllerState::BusOff ? "bus_off" :
                           ctl.state() == ControllerState::ErrorPassive ? "error_passive" : "error_active";
        assert(ctl.tec() == static_cast<unsigned>(std::stoul(tecStr)));
        assert(ctl.rec() == static_cast<unsigned>(std::stoul(recStr)));
        assert(expectedState == state && ctl.mayTransmit() == (mayTxStr == "1"));
    }

    // Classify observed samples and compare with codec metadata across DLCs,
    // identifiers, payload patterns, and stuffing at field boundaries.
    bool sawStuffed = false, sawData = false, sawZeroDlc = false;
    auto fieldName = [](Field f) -> const char * {
        switch (f) {
            case Field::SOF: return "SOF"; case Field::Arbitration: return "Arbitration";
            case Field::Control: return "Control"; case Field::Data: return "Data";
            case Field::CRC: return "CRC"; case Field::CRC_Delimiter: return "CRC_Delimiter";
            case Field::ACK: return "ACK"; case Field::ACK_Delimiter: return "ACK_Delimiter";
            case Field::EOF_Field: return "EOF"; case Field::Intermission: return "Intermission";
        }
        return "Unknown";
    };
    for (unsigned id : {0u, 0x123u, 0x600u, 0x601u, 0x7ffu}) for (unsigned dlc = 0; dlc <= 8; ++dlc) {
        CanFrame observed;
        observed.id = static_cast<std::uint16_t>(id);
        observed.dlc = static_cast<std::uint8_t>(dlc);
        for (unsigned i = 0; i < dlc; ++i) observed.data[i] = static_cast<std::uint8_t>(i * 37 + id);
        const auto bits = CanCodec::serialize(observed).bits;
        PassiveWireDecoder observer;
        for (const auto& bit : bits) {
            const auto result = observer.sample(bit.dominant);
            assert(result.valid);
            assert(std::string(result.field) == fieldName(bit.field));
            assert(result.logicalIndex == bit.logical_bit_index);
            assert(result.stuffed == bit.stuffed);
            sawStuffed |= bit.stuffed;
            sawData |= bit.field == Field::Data;
        }
        assert(observer.hasIdentifier() && observer.identifier() == id);
        sawZeroDlc |= dlc == 0;
        observer.reset();
        assert(!observer.hasIdentifier());
    }
    assert(sawStuffed && sawData && sawZeroDlc);
    PassiveWireDecoder invalidObserver;
    assert(!invalidObserver.sample(false).valid); // a frame must start with dominant SOF
    invalidObserver.reset();
    CanFrame stuffingFixture;
    const auto stuffingWire = CanCodec::serialize(stuffingFixture).bits;
    bool rejectedStuffing = false;
    for (std::size_t i = 0; i < stuffingWire.size(); ++i) {
        bool value = stuffingWire[i].dominant;
        if (stuffingWire[i].stuffed) value = stuffingWire[i - 1].dominant;
        if (!invalidObserver.sample(value).valid) { rejectedStuffing = true; break; }
    }
    assert(rejectedStuffing);
    CanFrame frame;
    frame.id = 0x123;
    frame.dlc = 2;
    frame.data[0] = 0x11;
    frame.data[1] = 0x22;
    auto wire = CanCodec::serialize(frame);
    assert(wire.crc == CanCodec::crc15(wire.unstuffed_crc_input));
    assert(wire.unstuffed_crc_input.size() == 1 + 11 + 1 + 1 + 1 + 4 + 16);
    assert(wire.crc_end > wire.crc_start);
    assert(wire.bits.size() > wire.unstuffed_crc_input.size() + 15);
    assert(wire.bits[0].field == Field::SOF && wire.bits[0].dominant);
    assert(wire.bits.back().field == Field::Intermission);
    for (std::size_t i = 1; i < wire.bits.size(); ++i) {
        if (wire.bits[i].stuffed) {
            assert(wire.bits[i].logical_bit_index == -1);
            assert(wire.bits[i - 1].dominant != wire.bits[i].dominant);
        }
    }

    CanFrame reference;
    reference.id = 0x601;
    reference.dlc = 4;
    reference.data[0] = 0x80;
    assert(CanCodec::serialize(reference).crc == 0x777a);
    reference = CanFrame{};
    reference.id = 0x7ff;
    assert(CanCodec::serialize(reference).crc == 0x272f);

    std::vector<bool> sampled;
    for (const auto& bit : wire.bits) sampled.push_back(bit.dominant);
    std::size_t ackIndex = 0;
    while (wire.bits[ackIndex].field != Field::ACK) ++ackIndex;
    sampled[ackIndex] = true; // a valid receiver drives dominant in the ACK slot
    auto decoded = CanCodec::decodeBusBits(sampled);
    assert(decoded.status == DecodeStatus::Valid);
    assert(decoded.frame.id == frame.id && decoded.frame.dlc == frame.dlc);
    assert(decoded.frame.data[0] == frame.data[0] && decoded.frame.data[1] == frame.data[1]);

    // Streaming receivers report at the first sampled point where an error is
    // observable, permit ACK on the following slot, and complete at EOF.
    CanReceiver streaming;
    bool sawAckEligible = false, sawComplete = false;
    std::size_t streamAckIndex = 0, streamCompleteIndex = 0;
    for (std::size_t i = 0; i < sampled.size(); ++i) {
        const auto event = streaming.sample(sampled[i], static_cast<std::int64_t>(i) * 1000);
        if (event.event == ReceiveEvent::AckEligible) {
            sawAckEligible = true;
            streamAckIndex = i;
            assert(event.status == DecodeStatus::Valid && event.timeNs == static_cast<std::int64_t>(i) * 1000);
        }
        if (event.event == ReceiveEvent::FrameComplete) {
            sawComplete = true;
            streamCompleteIndex = i;
            assert(event.frame.id == frame.id && event.frame.data[1] == frame.data[1]);
        }
        assert(event.event != ReceiveEvent::ErrorDetected);
    }
    assert(sawAckEligible && sawComplete);
    assert(streamAckIndex + 1 == ackIndex);
    std::size_t eofIndex = 0;
    while (wire.bits[eofIndex].field != Field::EOF_Field) ++eofIndex;
    assert(streamCompleteIndex == eofIndex + 6);

    auto detectFirst = [](const std::vector<bool>& bits, DecodeStatus expected) {
        CanReceiver receiver;
        for (std::size_t i = 0; i < bits.size(); ++i) {
            const auto event = receiver.sample(bits[i], static_cast<std::int64_t>(i) * 10);
            if (event.event == ReceiveEvent::ErrorDetected) {
                assert(event.status == expected);
                assert(event.wireBitIndex == i);
                assert(event.timeNs == static_cast<std::int64_t>(i) * 10);
                assert(receiver.failed());
                assert(receiver.sample(false, 999).event == ReceiveEvent::None);
                receiver.reset();
                assert(!receiver.failed() && !receiver.complete() && !receiver.ackEligible());
                return true;
            }
        }
        return false;
    };
    bool streamingStuffFault = false;
    for (std::size_t i = 1; i < sampled.size(); ++i) {
        if (!wire.bits[i].stuffed) continue;
        auto malformed = sampled;
        malformed[i] = malformed[i - 1];
        if (detectFirst(malformed, DecodeStatus::StuffError)) { streamingStuffFault = true; break; }
    }
    assert(streamingStuffFault);
    bool streamingCrcFault = false;
    for (std::size_t i = 0; i < sampled.size(); ++i) {
        if (wire.bits[i].field != Field::CRC || wire.bits[i].stuffed) continue;
        auto corrupt = sampled;
        corrupt[i] = !corrupt[i];
        if (detectFirst(corrupt, DecodeStatus::CRCError)) { streamingCrcFault = true; break; }
    }
    assert(streamingCrcFault);
    auto badDelimiter = sampled;
    badDelimiter[ackIndex - 1] = true;
    assert(detectFirst(badDelimiter, DecodeStatus::FormError));
    std::vector<bool> beforeAck(sampled.begin(), sampled.begin() + static_cast<std::ptrdiff_t>(ackIndex));
    assert(CanCodec::decodeBusBits(beforeAck, false).status == DecodeStatus::Valid);
    std::size_t intermissionStart = 0;
    while (wire.bits[intermissionStart].field != Field::Intermission) ++intermissionStart;
    std::vector<bool> throughEof(sampled.begin(), sampled.begin() + static_cast<std::ptrdiff_t>(intermissionStart));
    assert(CanCodec::decodeBusBits(throughEof, true, false).status == DecodeStatus::Valid);
    assert(CanCodec::decodeBusBits(throughEof).status == DecodeStatus::Truncated);

    auto noAck = sampled;
    noAck[ackIndex] = false;
    assert(CanCodec::decodeBusBits(noAck).status == DecodeStatus::AckError);
    auto badForm = sampled;
    badForm.back() = true;
    assert(CanCodec::decodeBusBits(badForm).status == DecodeStatus::FormError);
    auto truncated = sampled;
    truncated.pop_back();
    assert(CanCodec::decodeBusBits(truncated).status == DecodeStatus::Truncated);
    auto extra = sampled;
    extra.push_back(false);
    assert(CanCodec::decodeBusBits(extra).status == DecodeStatus::ExtraBits);

    bool checkedStuffFault = false;
    for (std::size_t i = 1; i < wire.bits.size(); ++i) {
        if (!wire.bits[i].stuffed) continue;
        auto malformed = sampled;
        malformed[i] = malformed[i - 1]; // repeat the run instead of inserting the complement
        if (CanCodec::decodeBusBits(malformed).status == DecodeStatus::StuffError) {
            checkedStuffFault = true;
            break;
        }
    }
    assert(checkedStuffFault);

    bool checkedCrcFault = false;
    for (std::size_t i = 0; i < wire.bits.size(); ++i) {
        if (wire.bits[i].field != Field::CRC || wire.bits[i].stuffed) continue;
        auto corrupt = sampled;
        corrupt[i] = !corrupt[i];
        if (CanCodec::decodeBusBits(corrupt).status == DecodeStatus::CRCError) {
            checkedCrcFault = true;
            break;
        }
    }
    assert(checkedCrcFault);

    CanFrame invalid = frame;
    invalid.id = 0x800;
    bool rejected = false;
    try { (void)CanCodec::serialize(invalid); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    invalid = frame; invalid.dlc = 9;
    rejected = false;
    try { (void)CanCodec::serialize(invalid); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);

    // At the first differing standard ID bit, the lower identifier wins.
    Bit recessive{false, Field::Arbitration, 4, false};
    CanController controller;
    assert(controller.observeTransmitBit(recessive, true) == TxObservation::ArbitrationLost);
    recessive.field = Field::Data;
    assert(controller.observeTransmitBit(recessive, true) == TxObservation::BitError);
    recessive.field = Field::ACK;
    assert(controller.observeTransmitBit(recessive, true) == TxObservation::Match);

    const auto bus = CanBus::resolve({Drive{1, false}, Drive{2, true}, Drive{3, false}});
    assert(bus.dominant && bus.dominant_drivers.size() == 1 && bus.dominant_drivers[0] == 2);
    assert(!CanBus::resolve({Drive{1, false}, Drive{2, false}}).dominant);

    for (int i = 0; i < 16; ++i) controller.transmitError(ErrorKind::Bit);
    assert(controller.tec() == 128 && controller.state() == ControllerState::ErrorPassive);
    controller.transmitError(ErrorKind::Ack);
    assert(controller.tec() == 128); // passive ACK exception
    controller.successfulTransmit();
    assert(controller.tec() == 127 && controller.state() == ControllerState::ErrorActive);
    for (int i = 0; i < 17; ++i) controller.transmitError(ErrorKind::Bit);
    assert(controller.state() == ControllerState::BusOff && controller.tec() == 256);
    const auto prior = controller.tec();
    controller.transmitError(ErrorKind::Bit);
    assert(controller.tec() == prior && !controller.mayTransmit() && !controller.mayAcknowledge());

    CanController receiver;
    for (int i = 0; i < 128; ++i) receiver.receiveError(ErrorKind::CRC);
    assert(receiver.rec() == 128 && receiver.state() == ControllerState::ErrorPassive);
    assert(receiver.suspendTransmissionBits() == 8);
    receiver.receiveError(ErrorKind::Bit, ReceiveErrorContext{true, false});
    assert(receiver.rec() == 136); // receiver bit error while sending active flag is +8
    receiver.successfulReceive();
    assert(receiver.rec() == 119 && receiver.state() == ControllerState::ErrorActive);
    assert(receiver.suspendTransmissionBits() == 0);

    CanController specialCases;
    for (int i = 0; i < 16; ++i) specialCases.transmitError(ErrorKind::Bit);
    specialCases.transmitError(ErrorKind::Ack, TransmitErrorContext{true, false});
    assert(specialCases.tec() == 136); // ACK error with dominant passive flag counts +8
    specialCases.transmitError(ErrorKind::Stuff, TransmitErrorContext{false, true});
    assert(specialCases.tec() == 136); // arbitration stuff error exception

    std::cout << "can core tests passed\n";
}
