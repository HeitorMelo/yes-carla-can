#include "CanBusModule.h"
#include "CanEvent_m.h"

#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>

using namespace omnetpp;

namespace canlab {
Define_Module(CanBusModule);

namespace {
std::string stateName(ControllerState state)
{
    switch (state) {
        case ControllerState::ErrorActive: return "error_active";
        case ControllerState::ErrorPassive: return "error_passive";
        case ControllerState::BusOff: return "bus_off";
    }
    return "unknown";
}

std::string fieldName(Field field)
{
    switch (field) {
        case Field::SOF: return "SOF";
        case Field::Arbitration: return "Arbitration";
        case Field::Control: return "Control";
        case Field::Data: return "Data";
        case Field::CRC: return "CRC";
        case Field::CRC_Delimiter: return "CRC_Delimiter";
        case Field::ACK: return "ACK";
        case Field::ACK_Delimiter: return "ACK_Delimiter";
        case Field::EOF_Field: return "EOF";
        case Field::Intermission: return "Intermission";
    }
    return "Unknown";
}
}

CanController& CanBusModule::controller(const std::string& ecu)
{
    return controllers.at(ecu);
}

void CanBusModule::requeueHead(const PendingFrame& frame)
{
    const auto nextForEcu = std::find_if(queue.begin(), queue.end(), [&](const PendingFrame& queued) {
        return queued.ecu == frame.ecu;
    });
    queue.insert(nextForEcu, frame);
}

void CanBusModule::initialize()
{
    bitDuration = SimTime(par("bitDuration"));
    if (bitDuration <= SIMTIME_ZERO) throw cRuntimeError("bitDuration must be positive (configured %s)", par("bitDuration").str().c_str());
    const auto configuredQueueDepth = par("maxQueueDepth").intValue();
    if (configuredQueueDepth <= 0) throw cRuntimeError("maxQueueDepth must be positive");
    maxQueueDepth = static_cast<size_t>(configuredQueueDepth);
    const auto names = cStringTokenizer(par("ecuNames").stringValue(), ",").asVector();
    for (const auto& name : names) {
        controllers.emplace(name, CanController{});
        receivers.emplace(name, CanReceiver{});
    }
    receiverNames = cStringTokenizer(par("receiverNames").stringValue(), ",").asVector();
    if (receiverNames.size() != static_cast<size_t>(gateSize("rxOut")))
        throw cRuntimeError("receiverNames count must match rxOut gate count");
    if (par("traceFile").stringValue()[0] != '\0') {
        trace.open(par("traceFile").stringValue(), std::ios::out | std::ios::trunc);
        if (!trace) throw cRuntimeError("cannot open trace output %s", par("traceFile").stringValue());
    }
    event = new cMessage("canBusEvent");
    event->setKind(StartBus);
}

void CanBusModule::acceptRequest(CanEvent *request)
{
    if (std::string(request->getEventType()) != "tx_request")
        throw cRuntimeError("bus txIn accepts only tx_request messages");
    PendingFrame pending;
    pending.ecu = request->getEcu();
    pending.uid = request->getFrameUid();
    if (!controllers.count(pending.ecu) || request->getCanId() < 0 || request->getCanId() > 0x7ff ||
        request->getDlc() < 0 || request->getDlc() > 8) {
        delete request;
        throw cRuntimeError("CAN request has unknown ECU or invalid standard ID/DLC");
    }
    pending.frame.id = static_cast<uint16_t>(request->getCanId());
    pending.frame.dlc = static_cast<uint8_t>(request->getDlc());
    for (int i = 0; i < 8; ++i) pending.frame.data[i] = request->getData(i);
    try { pending.frame.validate(); }
    catch (const std::exception& error) {
        delete request;
        throw cRuntimeError("invalid CAN request: %s", error.what());
    }
    auto& txController = controller(pending.ecu);
    if (!txController.mayTransmit()) {
        writeTrace("tx_blocked_bus_off", "controller_bus_off", -1, "", txController.tec(), txController.rec(), &pending);
        delete request;
        return;
    }
    if (queue.size() + contenders.size() >= maxQueueDepth) {
        writeTrace("tx_queue_reject", "queue_full_drop_newest", -1, "", txController.tec(), txController.rec(), &pending);
        delete request;
        return;
    }
    queue.push_back(pending);
    writeTrace("tx_queue_admit", "accepted", -1, "", txController.tec(), txController.rec(), &pending);
    delete request;
    if (!busy && !event->isScheduled()) {
        event->setKind(StartBus);
        // A one-nanosecond collection window makes simultaneous releases
        // deterministic before arbitration priority is selected.
        event->setSchedulingPriority(1);
        scheduleAt(simTime() + SimTime(1, SIMTIME_NS), event);
    }
}

void CanBusModule::beginAttempt()
{
    event->setSchedulingPriority(0);
    if (busy || queue.empty()) return;
    // Each controller contributes only its oldest queued application frame.
    // All such heads are independent bus drivers until bitwise arbitration
    // removes recessive contenders.
    std::set<std::string> included;
    simtime_t earliestEligible = SIMTIME_ZERO;
    for (auto it = queue.begin(); it != queue.end();) {
        if (!included.insert(it->ecu).second) { ++it; continue; }
        const auto eligibility = eligibleAt.find(it->ecu);
        if (eligibility != eligibleAt.end() && eligibility->second > simTime()) {
            if (earliestEligible == SIMTIME_ZERO || eligibility->second < earliestEligible)
                earliestEligible = eligibility->second;
            ++it;
            continue;
        }
        auto pending = *it;
        it = queue.erase(it);
        auto& txController = controller(pending.ecu);
        if (!txController.mayTransmit()) {
            writeTrace("tx_blocked_bus_off", "controller_bus_off", -1, "", txController.tec(), txController.rec(), &pending);
            continue;
        }
        ++pending.attempt;
        Contender contender;
        contender.pending = pending;
        try { contender.wire = CanCodec::serialize(pending.frame); }
        catch (const std::exception& error) { throw cRuntimeError("frame serialization failed: %s", error.what()); }
        contenders.push_back(std::move(contender));
    }
    if (contenders.empty()) {
        if (!queue.empty()) {
            event->setKind(StartBus);
            event->setSchedulingPriority(1);
            scheduleAt(earliestEligible > simTime() ? earliestEligible : simTime() + SimTime(1, SIMTIME_NS), event);
        }
        return;
    }

    // Identical IDs do not lose arbitration. Identical complete frames can
    // transmit in lockstep. A same-ID payload/DLC conflict is an explicitly
    // unsupported synchronized-transmitter case; fail loudly rather than
    // selecting one sender by insertion order.
    for (size_t i = 0; i < contenders.size(); ++i) {
        for (size_t j = i + 1; j < contenders.size(); ++j) {
            const auto& a = contenders[i].pending.frame;
            const auto& b = contenders[j].pending.frame;
            if (a.id != b.id) continue;
            bool identical = a.dlc == b.dlc;
            for (size_t byte = 0; identical && byte < a.dlc; ++byte)
                identical = a.data[byte] == b.data[byte];
            if (!identical) {
                const auto aEcu = contenders[i].pending.ecu;
                const auto bEcu = contenders[j].pending.ecu;
                const auto duplicateId = a.id;
                contenders.clear();
                throw cRuntimeError("unsupported simultaneous same-ID data conflict on 0x%03x from %s and %s; no sender was preselected",
                                    duplicateId, aEcu.c_str(), bEcu.c_str());
            }
        }
    }

    active = contenders.front().pending;
    wire = contenders.front().wire;
    sampledBusBits.clear();
    bitIndex = 0;
    busy = true;
    failed = false;
    deliveredFrame = false;
    attackDominant = false;
    for (const auto& contender : contenders) {
        const auto& txController = controller(contender.pending.ecu);
        writeTrace("tx_attempt", "wire_start", -1, "", txController.tec(), txController.rec(), &contender.pending);
    }
    beginBit();
}

void CanBusModule::beginBit()
{
    if (bitIndex >= wire.bits.size()) { finishFrame(); return; }
    attackDominant = attackNextBit;
    attackNextBit = false;
    bitStartTime = simTime();
    event->setKind(ResolveBit);
    // A wire observer acts on prior samples before this sampling event.
    scheduleAt(simTime() + SimTime(1, SIMTIME_NS), event);
}

void CanBusModule::resolveBit()
{
    if (contenders.empty()) throw cRuntimeError("CAN bus sampled a bit without an active transmitter");
    const auto& bit = contenders.front().wire.bits[bitIndex];
    const bool isAckSlot = bit.field == Field::ACK;
    std::vector<Drive> drives;
    drives.reserve(contenders.size() + 2);
    for (size_t i = 0; i < contenders.size(); ++i)
        drives.push_back(Drive{static_cast<unsigned>(i + 1), contenders[i].wire.bits[bitIndex].dominant});

    bool ack = false;
    if (isAckSlot && par("ackEnabled").boolValue()) {
        for (const auto& item : controllers) {
            const bool isTransmitter = std::any_of(contenders.begin(), contenders.end(), [&](const Contender& c) {
                return c.pending.ecu == item.first;
            });
            const auto receiver = receivers.find(item.first);
            if (!isTransmitter && item.second.mayAcknowledge() && receiver != receivers.end() && receiver->second.ackEligible()) {
                ack = true;
                break;
            }
        }
    }
    if (isAckSlot) drives.push_back(Drive{static_cast<unsigned>(contenders.size() + 1), ack});
    drives.push_back(Drive{static_cast<unsigned>(contenders.size() + 2), attackDominant});
    const auto resolution = CanBus::resolve(drives);
    sampledBusBits.push_back(resolution.dominant);
    if (attackDominant)
        writeTrace("attack_drive", "dominant_override", static_cast<int>(bitIndex), fieldName(bit.field).c_str());
    sendWireObservation("wire_sample", resolution.dominant);

    std::vector<ReceiverError> receiverErrors;
    for (const auto& ecu : receiverNames) {
        auto& rxController = controller(ecu);
        if (!rxController.mayAcknowledge()) continue;
        const auto sample = receivers.at(ecu).sample(resolution.dominant,
            static_cast<int64_t>((bitStartTime + SimTime(1, SIMTIME_NS)).inUnit(SIMTIME_NS)));
        const bool isTransmitter = std::any_of(contenders.begin(), contenders.end(), [&](const Contender& c) {
            return c.pending.ecu == ecu;
        });
        if (sample.event == ReceiveEvent::ErrorDetected && !isTransmitter)
            receiverErrors.push_back(ReceiverError{ecu, sample.status, sample.wireBitIndex});
        else if (sample.event == ReceiveEvent::FrameComplete && !isTransmitter) {
            const auto beforeTec = rxController.tec();
            const auto beforeRec = rxController.rec();
            rxController.successfulReceive();
            PendingFrame record = contenders.front().pending;
            record.ecu = ecu;
            record.frame = sample.frame;
            writeTrace("rx_accept", "incremental_frame_complete", static_cast<int>(sample.wireBitIndex), "EOF",
                       beforeTec, beforeRec, &record);
            if (ecu == "ActuatorGateway")
                writeTrace("actuator_update", "incremental_frame_complete", static_cast<int>(sample.wireBitIndex), "EOF",
                           beforeTec, beforeRec, &record);
            sendDelivery(static_cast<int>(std::find(receiverNames.begin(), receiverNames.end(), ecu) - receiverNames.begin()));
        }
    }

    std::vector<size_t> arbitrationLosers;
    std::vector<size_t> errorTransmitters;
    for (size_t i = 0; i < contenders.size(); ++i) {
        const auto& candidate = contenders[i];
        const auto& candidateBit = candidate.wire.bits[bitIndex];
        const auto& txController = controller(candidate.pending.ecu);
        const auto beforeTec = txController.tec();
        const auto beforeRec = txController.rec();
        const auto result = txController.observeTransmitBit(candidateBit, resolution.dominant);
        if (result == TxObservation::ArbitrationLost) {
            writeTrace("arbitration_lost", "recessive_vs_dominant", static_cast<int>(bitIndex),
                       fieldName(candidateBit.field).c_str(), beforeTec, beforeRec, &candidate.pending);
            requeueHead(candidate.pending);
            arbitrationLosers.push_back(i);
        } else if (result == TxObservation::BitError) {
            errorTransmitters.push_back(i);
        }
    }

    if (bit.field == Field::Arbitration) {
        const auto& traceFrame = contenders.front().pending;
        const auto& traceController = controller(traceFrame.ecu);
        writeTrace("arbitration_bit", resolution.dominant ? "dominant" : "recessive",
                   static_cast<int>(bitIndex), fieldName(bit.field).c_str(),
                   traceController.tec(), traceController.rec(), &traceFrame);
    }

    if (!errorTransmitters.empty() || !receiverErrors.empty()) {
        failAttempt(errorTransmitters.empty() ? "receiver_error" : "bit_error",
                    errorTransmitters, fieldName(bit.field).c_str(), receiverErrors);
        return;
    }
    for (auto it = arbitrationLosers.rbegin(); it != arbitrationLosers.rend(); ++it)
        contenders.erase(contenders.begin() + *it);
    if (contenders.empty()) throw cRuntimeError("all CAN transmitters lost arbitration on a resolved bus bit");

    active = contenders.front().pending;
    wire = contenders.front().wire;
    if (isAckSlot && !resolution.dominant) {
        std::vector<size_t> allTransmitters(contenders.size());
        for (size_t i = 0; i < allTransmitters.size(); ++i) allTransmitters[i] = i;
        failAttempt("ack_error", allTransmitters, "ACK");
        return;
    }
    if (bit.field == Field::EOF_Field && bitIndex + 1 < wire.bits.size() &&
        wire.bits[bitIndex + 1].field == Field::Intermission)
        deliverFrame();
    ++bitIndex;
    event->setKind(BeginBit);
    scheduleAt(bitStartTime + bitDuration, event);
}

void CanBusModule::finishFrame()
{
    const auto decoded = CanCodec::decodeBusBits(sampledBusBits);
    if (!decoded)
        throw cRuntimeError("resolved CAN frame failed final validation (status=%d); refusing delivery",
                            static_cast<int>(decoded.status));
    if (!deliveredFrame) throw cRuntimeError("valid frame ended without an EOF completion event");
    const auto& txController = controller(active.ecu);
    writeTrace("frame_wire", "resolved_samples_complete", -1, "", txController.tec(), txController.rec());
    contenders.clear();
    busy = false;
    attackNextBit = false;
    sendWireObservation("wire_idle");
    if (!queue.empty()) {
        event->setKind(StartBus);
        event->setSchedulingPriority(1);
        scheduleAt(simTime(), event);
    }
}

void CanBusModule::deliverFrame()
{
    const auto decoded = CanCodec::decodeBusBits(sampledBusBits, true, false);
    if (!decoded)
        throw cRuntimeError("resolved CAN frame failed EOF validation (status=%d); refusing delivery",
                            static_cast<int>(decoded.status));
    deliveredFrame = true;
    for (const auto& contender : contenders) {
        auto& txController = controller(contender.pending.ecu);
        const auto beforeTec = txController.tec();
        const auto beforeRec = txController.rec();
        txController.successfulTransmit();
        if (txController.state() == ControllerState::ErrorPassive)
            eligibleAt[contender.pending.ecu] = simTime() + bitDuration * 11; // intermission then suspend
        writeTrace("tx_success", "valid_frame_complete", -1, "", beforeTec, beforeRec, &contender.pending);
    }
    active = contenders.front().pending;
}

void CanBusModule::failAttempt(const char *reason, const std::vector<size_t>& errorTransmitters,
                               const char *errorField, const std::vector<ReceiverError>& receiverErrors)
{
    failed = true;
    attackNextBit = false;
    // Only controllers detecting the error initiate flags; an unrelated
    // error-active node cannot turn a passive flag into an active one.
    activeErrorFlag = false;
    for (const auto i : errorTransmitters)
        activeErrorFlag |= controller(contenders.at(i).pending.ecu).emitsActiveErrorFlag();
    for (const auto& error : receiverErrors)
        activeErrorFlag |= controller(error.ecu).emitsActiveErrorFlag();
    // This wrapper cannot yet model receivers continuing after an invisible,
    // passive-only flag. Fail explicitly rather than emit a misleading trace.
    if (!activeErrorFlag)
        throw cRuntimeError("unsupported passive-only error flag continuation; protocol run cannot be validated");
    const bool ackError = std::string(reason) == "ack_error";
    std::set<std::string> contendersByEcu;
    for (size_t i = 0; i < contenders.size(); ++i) {
        auto& contender = contenders[i];
        auto& txController = controller(contender.pending.ecu);
        contendersByEcu.insert(contender.pending.ecu);
        const auto beforeTec = txController.tec();
        const auto beforeRec = txController.rec();
        const bool detectedAsTransmitter = std::find(errorTransmitters.begin(), errorTransmitters.end(), i) != errorTransmitters.end();
        if (detectedAsTransmitter)
            txController.transmitError(ackError ? ErrorKind::Ack : ErrorKind::Bit);
        else if (activeErrorFlag)
            txController.transmitError(ErrorKind::Bit);
        PendingFrame record = contender.pending;
        writeTrace(detectedAsTransmitter ? (ackError ? "ack_error" : "bit_error") : "transmitter_aborted",
                   detectedAsTransmitter ? reason : "shared_error_flag_observed",
                   static_cast<int>(bitIndex), errorField, beforeTec, beforeRec, &record);
        if (txController.state() == ControllerState::BusOff)
            writeTrace("bus_off", "tec_exceeded_255", static_cast<int>(bitIndex), errorField, beforeTec, beforeRec, &record);
        if (txController.state() == ControllerState::ErrorPassive)
            suspendAfterError.insert(contender.pending.ecu);
        if (txController.mayTransmit()) {
            requeueHead(contender.pending);
            writeTrace("retry_scheduled", reason, static_cast<int>(bitIndex), errorField,
                       txController.tec(), txController.rec(), &record);
        }
    }
    // Receivers either detect the sampled error themselves or observe an
    // active flag. A passive flag alone is recessive and is not observable.
    for (auto& item : controllers) {
        if (contendersByEcu.count(item.first) || !item.second.mayAcknowledge()) continue;
        const auto detected = std::find_if(receiverErrors.begin(), receiverErrors.end(),
            [&](const ReceiverError& error) { return error.ecu == item.first; });
        if (detected == receiverErrors.end() && !activeErrorFlag) continue;
        const auto beforeTec = item.second.tec();
        const auto beforeRec = item.second.rec();
        ErrorKind kind = ErrorKind::Bit;
        if (detected != receiverErrors.end()) {
            if (detected->status == DecodeStatus::StuffError) kind = ErrorKind::Stuff;
            else if (detected->status == DecodeStatus::CRCError) kind = ErrorKind::CRC;
            else if (detected->status == DecodeStatus::FormError) kind = ErrorKind::Form;
        }
        item.second.receiveError(kind);
        PendingFrame record = active;
        record.ecu = item.first;
        writeTrace("receiver_error", detected != receiverErrors.end() ? "receiver_detected_error" : "shared_error_flag_observed", static_cast<int>(bitIndex), errorField,
                   beforeTec, beforeRec, &record);
    }
    if (!contenders.empty()) active = contenders.front().pending;
    contenders.clear();
    errorFlagBitsRemaining = 6;
    writeTrace("error_flag", activeErrorFlag ? "active_error_flag" : "passive_error_flag",
               static_cast<int>(bitIndex), errorField);
    event->setKind(ErrorFlagBit);
    scheduleAt(bitStartTime + bitDuration, event);
    (void)reason;
}

void CanBusModule::sendDelivery(int gateIndex)
{
    auto *delivery = new CanEvent("validCanDelivery");
    delivery->setEventType("rx_delivery");
    delivery->setEcu(active.ecu.c_str()); // trace provenance, not wire authentication
    delivery->setFrameUid(active.uid.c_str());
    delivery->setAttempt(static_cast<int>(active.attempt));
    delivery->setCanId(active.frame.id);
    delivery->setDlc(active.frame.dlc);
    for (int i = 0; i < 8; ++i) delivery->setData(i, active.frame.data[i]);
    send(delivery, "rxOut", gateIndex);
}

void CanBusModule::sendWireObservation(const char *type, bool dominant)
{
    auto *observation = new CanEvent("wireObservation");
    observation->setEventType(type);
    observation->setDominant(dominant);
    send(observation, "bitTap");
}

void CanBusModule::writeTrace(const char *name, const char *reason, int index, const char *field,
                              unsigned tecBefore, unsigned recBefore, const PendingFrame *frame)
{
    if (!trace) return;
    const PendingFrame& record = frame ? *frame : active;
    const auto& ctl = controller(record.ecu);
    std::ostringstream payload;
    payload << std::hex << std::setfill('0');
    for (unsigned i = 0; i < record.frame.dlc; ++i) payload << std::setw(2) << static_cast<unsigned>(record.frame.data[i]);
    const auto now = static_cast<int64_t>(simTime().inUnit(SIMTIME_NS));
    trace << "{\"time_ns\":" << now << ",\"run\":\"" << getEnvir()->getConfigEx()->getActiveConfigName()
          << "\",\"run_number\":" << getEnvir()->getConfigEx()->getActiveRunNumber()
          << ",\"seed\":" << par("experimentSeed").intValue()
          << ",\"ecu\":\"" << record.ecu << "\",\"frame_uid\":\"" << record.uid
          << "\",\"attempt\":" << record.attempt << ",\"can_id\":" << record.frame.id
          << ",\"dlc\":" << static_cast<unsigned>(record.frame.dlc) << ",\"payload_hex\":\"" << payload.str()
          << "\",\"event\":\"" << name << "\",\"reason\":\"" << reason
          << "\",\"field\":\"" << field << "\",\"bit_index\":" << index
          << ",\"tec_before\":" << tecBefore << ",\"tec_after\":" << ctl.tec()
          << ",\"rec_before\":" << recBefore << ",\"rec_after\":" << ctl.rec()
          << ",\"state\":\"" << stateName(ctl.state()) << "\",\"queue_depth\":" << queue.size();
    if (std::string(name) == "tx_success" || std::string(name) == "frame_wire") {
        trace << ",\"wire_samples\":\"";
        for (const bool dominant : sampledBusBits) trace << (dominant ? '1' : '0');
        trace << "\"";
    }
    trace << "}\n";
    trace.flush();
}

void CanBusModule::handleMessage(cMessage *message)
{
    if (!message->isSelfMessage()) {
        if (message->arrivedOn("txIn")) {
            acceptRequest(check_and_cast<CanEvent *>(message));
        } else if (message->arrivedOn("attackIn")) {
            auto *drive = check_and_cast<CanEvent *>(message);
            const bool validDrive = busy && !failed &&
                std::string(drive->getEventType()) == "attack_drive" && drive->getDominant();
            if (validDrive) attackNextBit = true;
            else writeTrace("attack_drive_rejected", "bus_not_sampling_frame", static_cast<int>(bitIndex));
            delete drive;
        }
        return;
    }
    switch (message->getKind()) {
        case StartBus: beginAttempt(); break;
        case BeginBit: beginBit(); break;
        case ResolveBit: resolveBit(); break;
        case ErrorFlagBit: {
            const auto resolution = CanBus::resolve({Drive{0, activeErrorFlag}});
            writeTrace("error_flag_bit", activeErrorFlag ? "dominant_error_active_flag" : "recessive_passive_flag",
                       static_cast<int>(6 - errorFlagBitsRemaining), "ErrorFlag");
            (void)resolution;
            if (--errorFlagBitsRemaining > 0) {
                event->setKind(ErrorFlagBit);
                scheduleAt(simTime() + bitDuration, event);
            } else {
                event->setKind(FinishError);
                scheduleAt(simTime() + bitDuration * 12, event); // final flag bit + delimiter + intermission
            }
            break;
        }
        case FinishError:
            for (auto& item : receivers) item.second.reset();
            sendWireObservation("wire_idle");
            for (const auto& ecu : suspendAfterError)
                if (controller(ecu).mayTransmit() && controller(ecu).state() == ControllerState::ErrorPassive)
                    eligibleAt[ecu] = simTime() + bitDuration * 8;
            suspendAfterError.clear();
            failed = false;
            busy = false;
            if (!queue.empty()) {
                event->setKind(StartBus);
                event->setSchedulingPriority(1);
                scheduleAt(simTime(), event);
            }
            else busy = false;
            break;
        default: throw cRuntimeError("unknown CAN bus self-message kind");
    }
}

void CanBusModule::finish()
{
    if (event) cancelAndDelete(event);
    event = nullptr;
    if (trace.is_open()) trace.close();
}

} // namespace canlab
