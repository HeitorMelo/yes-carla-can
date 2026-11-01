#include "ActuatorGateway.h"
#include "CanEvent_m.h"

#include <cstdint>
#include <string>

using namespace omnetpp;

namespace canlab {

Define_Module(ActuatorGateway);

namespace {
constexpr uint16_t BRAKE_CAN_ID = 0x601;
}

void ActuatorGateway::initialize()
{
    freshnessTimeout = SimTime(par("freshnessTimeout"));
    if (freshnessTimeout <= SIMTIME_ZERO)
        throw cRuntimeError("freshnessTimeout must be positive");

    freshness.open(par("freshnessFile").stringValue(), std::ios::out | std::ios::trunc);
    if (!freshness)
        throw cRuntimeError("cannot open freshness output");

    freshnessTimer = new cMessage("brakeFreshnessTimeout");
}

void ActuatorGateway::handleMessage(cMessage *message)
{
    if (message == freshnessTimer) {
        const auto age = simTime() - lastBrakeDelivery;
        writeFreshness("stale", age);
        return;
    }

    auto *event = check_and_cast<CanEvent *>(message);
    if (std::string(event->getEventType()) != "rx_delivery")
        throw cRuntimeError("ActuatorGateway accepts only validated bus deliveries");

    emit(registerSignal("actuatorUpdate"), static_cast<long>(event->getCanId()));
    if (event->getCanId() == BRAKE_CAN_ID) {
        lastBrakeDelivery = simTime();
        writeFreshness("fresh", SIMTIME_ZERO);

        if (freshnessTimer->isScheduled())
            cancelEvent(freshnessTimer);
        scheduleAt(simTime() + freshnessTimeout, freshnessTimer);
    }

    delete event;
}

void ActuatorGateway::writeFreshness(const char *state, simtime_t age)
{
    freshness << "{\"time_ns\":" << simTime().inUnit(SIMTIME_NS)
              << ",\"state\":\"" << state << "\",\"can_id\":" << BRAKE_CAN_ID
              << ",\"age_ns\":" << age.inUnit(SIMTIME_NS) << "}\n";
}

void ActuatorGateway::finish()
{
    cancelAndDelete(freshnessTimer);
    freshnessTimer = nullptr;
    freshness.close();
}

} // namespace canlab
