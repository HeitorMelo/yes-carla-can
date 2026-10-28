#include "SyntheticEcuApp.h"
#include "CanEvent_m.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace omnetpp;

namespace canlab {

Define_Module(SyntheticEcuApp);

namespace {
std::vector<std::string> split(const std::string& text, char delimiter)
{
    std::vector<std::string> result;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, delimiter)) result.push_back(item);
    return result;
}

uint8_t nibble(char c)
{
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    throw cRuntimeError("invalid hexadecimal payload character '%c'", c);
}
}

void SyntheticEcuApp::parseFrameSpecs(const char *raw)
{
    for (const auto& entry : split(raw, ';')) {
        if (entry.empty()) continue;
        const auto fields = split(entry, ',');
        if (fields.size() != 5)
            throw cRuntimeError("%s frameSpecs entry must have five comma-separated fields", ecuName.c_str());
        FrameSpec spec{};
        try {
            const auto id = std::stoul(fields[0], nullptr, 0);
            spec.periodNs = std::stoll(fields[1]);
            spec.phaseNs = std::stoll(fields[2]);
            const auto dlc = std::stoul(fields[3]);
            if (id > 0x7ff || dlc > 8 || spec.periodNs < 0 || spec.phaseNs < 0)
                throw cRuntimeError("%s has invalid standard CAN ID, DLC, or schedule", ecuName.c_str());
            spec.id = static_cast<uint16_t>(id);
            spec.dlc = static_cast<uint8_t>(dlc);
            std::string payload = fields[4];
            if (payload.size() != static_cast<size_t>(2 * spec.dlc))
                throw cRuntimeError("%s CAN ID 0x%03x payload length does not match DLC", ecuName.c_str(), spec.id);
            for (size_t i = 0; i < spec.dlc; ++i)
                spec.data[i] = static_cast<uint8_t>((nibble(payload[2 * i]) << 4) | nibble(payload[2 * i + 1]));
        } catch (const cRuntimeError&) {
            throw;
        } catch (const std::exception& error) {
            throw cRuntimeError("invalid frameSpecs for %s: %s", ecuName.c_str(), error.what());
        }
        frames.push_back(spec);
    }
    if (frames.empty()) throw cRuntimeError("%s has no synthetic frame specifications", ecuName.c_str());
}

void SyntheticEcuApp::initialize()
{
    ecuName = par("ecuName").stdstringValue();
    if (par("maxReleases").intValue() == 0 || par("maxReleases").intValue() < -1)
        throw cRuntimeError("%s maxReleases must be -1 or a positive number", ecuName.c_str());
    parseFrameSpecs(par("frameSpecs").stringValue());
    releaseTimers.reserve(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        auto *timer = new cMessage("applicationRelease");
        timer->setSchedulingPriority(par("releasePriority").intValue());
        timer->setContextPointer(reinterpret_cast<void *>(i + 1));
        releaseTimers.push_back(timer);
        scheduleAt(SimTime(frames[i].phaseNs, SIMTIME_NS), timer);
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
    for (int i = 0; i < 8; ++i) event->setData(i, spec.data[i]);
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
    if (spec.periodNs > 0 && (maxReleases < 0 || spec.sequence < static_cast<unsigned>(maxReleases))) {
        // Absolute schedule prevents accumulated timer drift.
        spec.phaseNs += spec.periodNs;
        scheduleAt(SimTime(spec.phaseNs, SIMTIME_NS), message);
    } else {
        delete message;
        releaseTimers[index] = nullptr;
    }
}

void SyntheticEcuApp::finish()
{
    for (auto *timer : releaseTimers)
        if (timer) cancelAndDelete(timer);
    releaseTimers.clear();
}

} // namespace canlab
