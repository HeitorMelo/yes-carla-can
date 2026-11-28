#ifndef CANLAB_ACTUATORGATEWAY_H
#define CANLAB_ACTUATORGATEWAY_H

#include <omnetpp.h>
#include <fstream>

namespace canlab {

// Application sink for valid committed frames. The bus/controller layer owns
// ACK behavior; this module only represents delivered application commands.
class ActuatorGateway : public omnetpp::cSimpleModule {
    omnetpp::cMessage *freshnessTimer = nullptr;
    omnetpp::simtime_t lastBrakeDelivery;
    omnetpp::simtime_t freshnessTimeout;
    std::ofstream freshness;

    void writeFreshness(const char *state, omnetpp::simtime_t age);

  protected:
    void initialize() override;
    void handleMessage(omnetpp::cMessage *message) override;
    void finish() override;
};

} // namespace canlab

#endif
