#ifndef CANLAB_SYNTHETICECUAPP_H
#define CANLAB_SYNTHETICECUAPP_H

#include <omnetpp.h>
#include <cstdint>
#include <string>
#include <vector>

namespace canlab {

// Periodic and one-shot synthetic application traffic. Each frame specification
// is `id,period_ns,phase_ns,dlc,payload_hex`; period 0 is a one-shot event.
// Queueing and wire/controller behavior belong to the connected CAN module.
class SyntheticEcuApp : public omnetpp::cSimpleModule {
  private:
    struct FrameSpec {
        uint16_t id;
        int64_t periodNs;
        int64_t phaseNs;
        uint8_t dlc;
        uint8_t data[8];
        uint64_t sequence = 0;
    };

    std::string ecuName;
    std::vector<FrameSpec> frames;
    std::vector<omnetpp::cMessage *> releaseTimers;

    void parseFrameSpecs(const char *specs);
    void releaseFrame(FrameSpec& spec);

  protected:
    void initialize() override;
    void handleMessage(omnetpp::cMessage *message) override;
    void finish() override;
};

} // namespace canlab

#endif
