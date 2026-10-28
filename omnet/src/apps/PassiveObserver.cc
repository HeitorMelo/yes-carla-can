#include "../can/PassiveWireDecoder.h"
#include "CanEvent_m.h"
#include <omnetpp.h>
#include <fstream>
#include <string>

using namespace omnetpp;

namespace canlab {
class PassiveObserver : public cSimpleModule {
    PassiveWireDecoder decoder;
    std::ofstream output;
    simtime_t previous;
    bool first = true;
    bool enabled = false;
    unsigned frame = 0, bit = 0;
  protected:
    void initialize() override {
        enabled = par("enabled").boolValue();
        if (enabled) {
            output.open(par("outputFile").stringValue(), std::ios::out | std::ios::trunc);
            if (!output) throw cRuntimeError("cannot open passive observer output");
        }
    }
    void handleMessage(cMessage *message) override {
        auto *sample = check_and_cast<CanEvent *>(message);
        const std::string type = sample->getEventType();
        if (enabled && type == "wire_idle") {
            decoder.reset(); first = true; bit = 0; ++frame;
        } else if (enabled && type == "wire_sample") {
            const auto observation = decoder.sample(sample->getDominant());
            const auto now = static_cast<int64_t>(simTime().inUnit(SIMTIME_NS));
            const auto interval = first ? 0 : static_cast<int64_t>((simTime() - previous).inUnit(SIMTIME_NS));
            output << "{\"time_ns\":" << now << ",\"interval_ns\":" << interval
                   << ",\"frame\":" << frame << ",\"wire_index\":" << bit++
                   << ",\"dominant\":" << (sample->getDominant() ? "true" : "false")
                   << ",\"field\":\"" << observation.field << "\",\"logical_index\":" << observation.logicalIndex
                   << ",\"stuffed\":" << (observation.stuffed ? "true" : "false")
                   << ",\"valid\":" << (observation.valid ? "true" : "false")
                   << ",\"can_id\":" << (decoder.hasIdentifier() ? static_cast<int>(decoder.identifier()) : -1) << "}\n";
            previous = simTime(); first = false;
        }
        delete sample;
    }
};
Define_Module(PassiveObserver);
} // namespace canlab
