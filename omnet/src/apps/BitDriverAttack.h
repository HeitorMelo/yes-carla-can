#ifndef CANLAB_BITDRIVERATTACK_H
#define CANLAB_BITDRIVERATTACK_H

#include <omnetpp.h>
#include "../can/WireObserver.h"

namespace canlab {

// A compromised transceiver that observes a sampled wire position and can
// independently pull the bus dominant. It never sees controller counters.
class BitDriverAttack : public omnetpp::cSimpleModule {
  protected:
    int targetCanId = 0;
    int targetLogicalBit = 0;
    int maxInterventions = 0;
    int interventions = 0;
    omnetpp::simtime_t startAt;
    omnetpp::simtime_t stopAt;
    WireObserver observer;
    void initialize() override;
    void handleMessage(omnetpp::cMessage *message) override;
};

} // namespace canlab

#endif
