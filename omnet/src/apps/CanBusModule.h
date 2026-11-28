#ifndef CANLAB_CANBUSMODULE_H
#define CANLAB_CANBUSMODULE_H

#include "../can/CanBus.h"
#include "../can/CanController.h"
#include <deque>
#include <fstream>
#include <omnetpp.h>
#include <set>
#include <unordered_map>
#include <vector>

namespace canlab {

class CanEvent;

class CanBusModule : public omnetpp::cSimpleModule {
  private:
    struct PendingFrame {
        CanFrame frame;
        std::string ecu;
        std::string uid;
        unsigned attempt = 0;
    };
    struct Contender {
        PendingFrame pending;
        WireFrame wire;
    };
    struct ReceiverError {
        std::string ecu;
        DecodeStatus status = DecodeStatus::NeedMore;
        size_t bit = 0;
    };
    enum SelfKind { StartBus = 1, BeginBit = 2, ResolveBit = 3, ErrorFlagBit = 4, FinishError = 5 };

    std::unordered_map<std::string, CanController> controllers;
    std::unordered_map<std::string, CanReceiver> receivers;
    std::unordered_map<std::string, omnetpp::simtime_t> eligibleAt;
    std::set<std::string> suspendAfterError;
    std::vector<std::string> receiverNames;
    std::deque<PendingFrame> queue;
    std::vector<Contender> contenders;
    PendingFrame active{};
    WireFrame wire{};
    std::vector<bool> sampledBusBits;
    size_t bitIndex = 0;
    omnetpp::simtime_t bitStartTime;
    bool attackDominant = false;
    bool attackNextBit = false;
    bool busy = false;
    bool failed = false;
    bool deliveredFrame = false;
    unsigned errorFlagBitsRemaining = 0;
    bool activeErrorFlag = false;
    size_t maxQueueDepth = 0;
    omnetpp::simtime_t bitDuration;
    omnetpp::cMessage* event = nullptr;
    std::ofstream trace;

    CanController& controller(const std::string& ecu);
    bool isTransmitter(const std::string& ecu) const;
    bool acknowledgementPresent() const;
    std::vector<ReceiverError> sampleReceivers(bool dominant);
    void validateSameIdContenders() const;
    void requeueHead(const PendingFrame& frame);
    void acceptRequest(CanEvent* request);
    void beginAttempt();
    void beginBit();
    void resolveBit();
    void finishFrame();
    void deliverFrame();
    void failAttempt(const char* reason, const std::vector<size_t>& errorTransmitters = {},
                     const char* errorField = "ErrorFlag",
                     const std::vector<ReceiverError>& receiverErrors = {});
    void writeTrace(const char* eventName, const char* reason, int bit = -1, const char* field = "",
                    unsigned tecBefore = 0, unsigned recBefore = 0, const PendingFrame* frame = nullptr);
    void sendDelivery(int gateIndex);
    void sendWireObservation(const char* type, bool dominant = false);

  protected:
    void initialize() override;
    void handleMessage(omnetpp::cMessage* message) override;
    void finish() override;
};

} // namespace canlab

#endif
