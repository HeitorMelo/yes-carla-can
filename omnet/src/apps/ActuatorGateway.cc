#include "ActuatorGateway.h"
#include "CanEvent_m.h"

#include <string>

using namespace omnetpp;

namespace canlab {
Define_Module(ActuatorGateway);

void ActuatorGateway::initialize()
{
    freshnessTimeout = SimTime(par("freshnessTimeout"));
    if (freshnessTimeout <= SIMTIME_ZERO) throw cRuntimeError("freshnessTimeout must be positive");
    freshness.open(par("freshnessFile").stringValue(), std::ios::out | std::ios::trunc);
    if (!freshness) throw cRuntimeError("cannot open freshness output");
    freshnessTimer = new cMessage("brakeFreshnessTimeout");
}

void ActuatorGateway::handleMessage(cMessage *message)
{
    if (message == freshnessTimer) {
        freshness << "{\"time_ns\":" << simTime().inUnit(SIMTIME_NS)
                  << ",\"state\":\"stale\",\"can_id\":1537,\"age_ns\":"
                  << (simTime() - lastBrakeDelivery).inUnit(SIMTIME_NS) << "}\n";
        return;
    }
    auto *event = check_and_cast<CanEvent *>(message);
    if (std::string(event->getEventType()) != "rx_delivery")
        throw cRuntimeError("ActuatorGateway accepts only validated bus deliveries");
    emit(registerSignal("actuatorUpdate"), static_cast<long>(event->getCanId()));
    if (event->getCanId() == 0x601) {
        lastBrakeDelivery = simTime();
        freshness << "{\"time_ns\":" << simTime().inUnit(SIMTIME_NS)
                  << ",\"state\":\"fresh\",\"can_id\":1537,\"age_ns\":0}\n";
        if (freshnessTimer->isScheduled()) cancelEvent(freshnessTimer);
        scheduleAt(simTime() + freshnessTimeout, freshnessTimer);
    }
    delete event;
}

void ActuatorGateway::finish()
{
    cancelAndDelete(freshnessTimer);
    freshnessTimer = nullptr;
    freshness.close();
}

} // namespace canlab
