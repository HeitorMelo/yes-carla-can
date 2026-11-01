#include "../can/PassiveWireDecoder.h"
#include "CanEvent_m.h"

#include <omnetpp.h>

#include <fstream>
#include <string>

using namespace omnetpp;

namespace canlab {

class PassiveObserver : public cSimpleModule {
  private:
    PassiveWireDecoder decoder;
    std::ofstream output;
    simtime_t previousSample;
    bool firstSample = true;
    bool enabled = false;
    unsigned frameNumber = 0;
    unsigned wireIndex = 0;

    void recordSample(const CanEvent& sample);

  protected:
    void initialize() override;
    void handleMessage(cMessage *message) override;
};

Define_Module(PassiveObserver);

void PassiveObserver::initialize()
{
    enabled = par("enabled").boolValue();
    if (!enabled)
        return;

    output.open(par("outputFile").stringValue(), std::ios::out | std::ios::trunc);
    if (!output)
        throw cRuntimeError("cannot open passive observer output");
}

void PassiveObserver::handleMessage(cMessage *message)
{
    auto *sample = check_and_cast<CanEvent *>(message);
    const std::string type = sample->getEventType();

    if (enabled && type == "wire_idle") {
        decoder.reset();
        firstSample = true;
        wireIndex = 0;
        ++frameNumber;
    }
    else if (enabled && type == "wire_sample") {
        recordSample(*sample);
    }

    delete sample;
}

void PassiveObserver::recordSample(const CanEvent& sample)
{
    const auto observation = decoder.sample(sample.getDominant());
    const auto now = static_cast<int64_t>(simTime().inUnit(SIMTIME_NS));
    const auto interval = firstSample
        ? 0
        : static_cast<int64_t>((simTime() - previousSample).inUnit(SIMTIME_NS));

    output << "{\"time_ns\":" << now
           << ",\"interval_ns\":" << interval
           << ",\"frame\":" << frameNumber
           << ",\"wire_index\":" << wireIndex++
           << ",\"dominant\":" << (sample.getDominant() ? "true" : "false")
           << ",\"field\":\"" << observation.field << "\""
           << ",\"logical_index\":" << observation.logicalIndex
           << ",\"stuffed\":" << (observation.stuffed ? "true" : "false")
           << ",\"valid\":" << (observation.valid ? "true" : "false")
           << ",\"can_id\":"
           << (decoder.hasIdentifier() ? static_cast<int>(decoder.identifier()) : -1)
           << "}\n";

    previousSample = simTime();
    firstSample = false;
}

} // namespace canlab
