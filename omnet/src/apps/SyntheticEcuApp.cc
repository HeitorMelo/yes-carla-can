#include "SyntheticEcuApp.h"
#include "CanEvent_m.h"

#include <sstream>
#include <stdexcept>

using namespace omnetpp;

namespace canlab {

Define_Module(SyntheticEcuApp);

namespace {

std::vector<std::string> split(const std::string& text, char delimiter)
{
    std::vector<std::string> fields;
    std::stringstream stream(text);
    std::string field;

    while (std::getline(stream, field, delimiter))
        fields.push_back(field);

    return fields;
}

uint8_t parseHexDigit(char digit)
{
    if (digit >= '0' && digit <= '9')
        return static_cast<uint8_t>(digit - '0');
    if (digit >= 'a' && digit <= 'f')
        return static_cast<uint8_t>(digit - 'a' + 10);
    if (digit >= 'A' && digit <= 'F')
        return static_cast<uint8_t>(digit - 'A' + 10);

    throw cRuntimeError("invalid hexadecimal payload character '%c'", digit);
}

} // namespace

void SyntheticEcuApp::parseFrameSpecs(const char *raw)
{
    for (const auto& entry : split(raw, ';')) {
        if (entry.empty())
            continue;

        const auto fields = split(entry, ',');
        if (fields.size() != 5)
            throw cRuntimeError("%s frameSpecs entry must have five comma-separated fields",
                                ecuName.c_str());

        FrameSpec spec{};
        try {
            const auto id = std::stoul(fields[0], nullptr, 0);
            spec.periodNs = std::stoll(fields[1]);
            spec.phaseNs = std::stoll(fields[2]);
            const auto dlc = std::stoul(fields[3]);

            if (id > 0x7ff || dlc > 8 || spec.periodNs < 0 || spec.phaseNs < 0)
                throw cRuntimeError("%s has invalid standard CAN ID, DLC, or schedule",
                                    ecuName.c_str());

            spec.id = static_cast<uint16_t>(id);
            spec.dlc = static_cast<uint8_t>(dlc);
            const std::string& payload = fields[4];
            if (payload.size() != 2 * spec.dlc)
                throw cRuntimeError("%s CAN ID 0x%03x payload length does not match DLC",
                                    ecuName.c_str(), spec.id);

            for (size_t byte = 0; byte < spec.dlc; ++byte) {
                const size_t highDigit = 2 * byte;
                spec.data[byte] = static_cast<uint8_t>(
                    (parseHexDigit(payload[highDigit]) << 4) |
                    parseHexDigit(payload[highDigit + 1]));
            }
        }
        catch (const cRuntimeError&) {
            throw;
        }
        catch (const std::exception& error) {
            throw cRuntimeError("invalid frameSpecs for %s: %s",
                                ecuName.c_str(), error.what());
        }

        frames.push_back(spec);
    }

    if (frames.empty())
        throw cRuntimeError("%s has no synthetic frame specifications", ecuName.c_str());
}

void SyntheticEcuApp::initialize()
{
    ecuName = par("ecuName").stdstringValue();

    const auto maxReleases = par("maxReleases").intValue();
    if (maxReleases == 0 || maxReleases < -1)
        throw cRuntimeError("%s maxReleases must be -1 or a positive number", ecuName.c_str());

    parseFrameSpecs(par("frameSpecs").stringValue());
    releaseTimers.reserve(frames.size());

    for (size_t index = 0; index < frames.size(); ++index) {
        auto *timer = new cMessage("applicationRelease");
        timer->setSchedulingPriority(par("releasePriority").intValue());
        timer->setContextPointer(reinterpret_cast<void *>(index + 1));
        releaseTimers.push_back(timer);
        scheduleAt(SimTime(frames[index].phaseNs, SIMTIME_NS), timer);
    }
}

void SyntheticEcuApp::releaseFrame(FrameSpec& spec)
{
    auto *event = new CanEvent("applicationRelease");
    event->setEventType("tx_request");
    event->setEcu(ecuName.c_str());

    std::ostringstream uid;
    uid << ecuName << ':' << std::hex << spec.id << ':' << std::dec << spec.sequence++;
    event->setFrameUid(uid.str().c_str());
    event->setCanId(spec.id);
    event->setDlc(spec.dlc);

    for (int byte = 0; byte < 8; ++byte)
        event->setData(byte, spec.data[byte]);

    send(event, "tx");
    emit(registerSignal("applicationRelease"), static_cast<long>(spec.id));
}

void SyntheticEcuApp::handleMessage(cMessage *message)
{
    if (!message->isSelfMessage()) {
        auto *event = check_and_cast<CanEvent *>(message);
        if (std::string(event->getEventType()) == "rx_delivery")
            emit(registerSignal("applicationReceive"), static_cast<long>(event->getCanId()));
        delete event;
        return;
    }

    const auto rawIndex = reinterpret_cast<std::uintptr_t>(message->getContextPointer());
    if (rawIndex == 0 || rawIndex > frames.size())
        throw cRuntimeError("%s received an unexpected message", ecuName.c_str());

    const size_t index = rawIndex - 1;
    auto& spec = frames[index];
    releaseFrame(spec);

    const auto maxReleases = par("maxReleases").intValue();
    if (spec.periodNs > 0 &&
        (maxReleases < 0 || spec.sequence < static_cast<unsigned>(maxReleases))) {
        // Advance the scheduled phase to avoid timer drift.
        spec.phaseNs += spec.periodNs;
        scheduleAt(SimTime(spec.phaseNs, SIMTIME_NS), message);
    }
    else {
        delete message;
        releaseTimers[index] = nullptr;
    }
}

void SyntheticEcuApp::finish()
{
    for (auto *timer : releaseTimers) {
        if (timer)
            cancelAndDelete(timer);
    }
    releaseTimers.clear();
}

} // namespace canlab
