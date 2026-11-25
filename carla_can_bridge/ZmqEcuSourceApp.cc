#include <omnetpp.h>
#include <zmq.h>

#include "fico4omnet/applications/can/source/CanTrafficSourceAppBase.h"
#include "fico4omnet/buffer/can/CanOutputBuffer.h"
#include "fico4omnet/bus/can/CanBusLogic.h"
#include "fico4omnet/linklayer/can/messages/ErrorFrame_m.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <sstream>
#include <set>
#include <string>
#include <vector>

using namespace omnetpp;
using namespace FiCo4OMNeT;

namespace {
bool decodeHex(const std::string& text, std::vector<unsigned char>& bytes) {
    if ((text.size() % 2) != 0 || text.size() > 16)
        return false;
    bytes.clear();
    for (size_t i = 0; i < text.size(); i += 2) {
        const unsigned char hi = static_cast<unsigned char>(text[i]);
        const unsigned char lo = static_cast<unsigned char>(text[i + 1]);
        if (!std::isxdigit(hi) || !std::isxdigit(lo))
            return false;
        bytes.push_back(static_cast<unsigned char>(std::stoul(text.substr(i, 2), nullptr, 16)));
    }
    return true;
}

unsigned int approximateFrameBits(size_t payloadBytes) {
    // FiCo4OMNeT represents CAN frames by a frame duration and an arbitration
    // ID. This gives it the correct payload-dependent duration but does not
    // synthesize the actual CRC/stuffed bit stream.
    const unsigned int unstuffedBits = 47 + static_cast<unsigned int>(payloadBytes * 8);
    const unsigned int stuffableBits = 34 + 11 + static_cast<unsigned int>(payloadBytes * 8);
    const unsigned int estimatedStuffBits = (stuffableBits / 4) * 15 / 100;
    return unstuffedBits + estimatedStuffBits;
}
}

class ZmqEcuSourceApp : public CanTrafficSourceAppBase {
  private:
    void *context = nullptr;
    void *socket = nullptr;
    cMessage *pollTimer = nullptr;
    std::string topic;
    int ecuIndex = -1;
    int tec = 0;
    int rec = 0;
    bool busOff = false;
    std::set<unsigned int> queuedCanIds;

    void discardQueuedFrames() {
        CanOutputBuffer *output = check_and_cast<CanOutputBuffer *>(
            getParentModule()->getSubmodule("bufferOut"));
        CanBusLogic *bus = check_and_cast<CanBusLogic *>(
            getParentModule()->getParentModule()->getSubmodule("canBus")
                ->getSubmodule("canBusLogic"));

        if (CanDataFrame *current = output->getCurrentFrame()) {
            bus->checkoutFromArbitration(current->getCanID());
            output->sendingCompleted();
        }
        for (unsigned int canId : queuedCanIds) {
            if (CanDataFrame *queued = output->getFrame(canId)) {
                bus->checkoutFromArbitration(canId);
                output->deleteFrame(queued);
            }
        }
        queuedCanIds.clear();
    }

    void setupSocket() {
        context = zmq_ctx_new();
        if (!context)
            throw cRuntimeError("Cannot create ZeroMQ context: %s", zmq_strerror(errno));
        socket = zmq_socket(context, ZMQ_SUB);
        if (!socket)
            throw cRuntimeError("Cannot create ZeroMQ SUB socket: %s", zmq_strerror(errno));

        const int linger = 0;
        zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
        const char *endpoint = par("inputEndpoint").stringValue();
        const char *filter = topic.c_str();
        if (zmq_setsockopt(socket, ZMQ_SUBSCRIBE, filter, topic.size()) != 0 ||
            zmq_connect(socket, endpoint) != 0)
            throw cRuntimeError("Cannot connect %s subscriber to %s: %s",
                                topic.c_str(), endpoint, zmq_strerror(errno));
    }

    void enqueueFrame(const std::string& wireMessage) {
        std::istringstream input(wireMessage);
        std::string receivedTopic;
        std::string payloadHex;
        unsigned int canId = 0;
        if (!(input >> receivedTopic >> canId >> payloadHex) || receivedTopic != topic)
            return;

        std::vector<unsigned char> payloadBytes;
        if (!decodeHex(payloadHex, payloadBytes)) {
            EV_WARN << "Ignoring malformed CAN payload from ZeroMQ: " << wireMessage << "\n";
            return;
        }
        if (busOff)
            return;

        CanDataFrame *frame = new CanDataFrame("carlaCanFrame");
        frame->setCanID(canId);
        frame->setRtr(false);
        frame->setPeriod(0.0);
        frame->setKind(ecuIndex);
        frame->setTimestamp(simTime());

        std::string payloadName = payloadHex;
        cPacket *payload = new cPacket(payloadName.c_str());
        payload->setByteLength(static_cast<int>(payloadBytes.size()));
        frame->encapsulate(payload);
        // encapsulate() adds payload bits to the packet length; override that
        // with the complete estimated CAN frame length for FiCo's timing model.
        frame->setBitLength(approximateFrameBits(payloadBytes.size()));
        queuedCanIds.insert(canId);
        EV_INFO << topic << " queued CAN ID " << canId << " payload " << payloadHex << "\n";
        send(frame, "out");
    }

    void pollPublisher() {
        char buffer[256];
        while (true) {
            const int length = zmq_recv(socket, buffer, sizeof(buffer), ZMQ_DONTWAIT);
            if (length < 0) {
                if (errno == EAGAIN)
                    break;
                throw cRuntimeError("ZeroMQ receive failed for %s: %s", topic.c_str(), zmq_strerror(errno));
            }
            if (static_cast<size_t>(length) >= sizeof(buffer)) {
                EV_WARN << "Ignoring oversized ZeroMQ CAN message for " << topic << "\n";
                continue;
            }
            const std::string wireMessage(buffer, static_cast<size_t>(length));
            EV_INFO << topic << " received " << wireMessage << "\n";
            enqueueFrame(wireMessage);
        }
    }

    void handleConfinementError(ErrorFrame *error) {
        if (error->getNode() == ecuIndex) {
            tec = std::min(256, tec + 8);
            EV_WARN << topic << " TEC=" << tec << " after active error on CAN ID "
                    << error->getCanID() << "\n";
            if (tec > 255 && !busOff) {
                busOff = true;
                EV_ERROR << topic << " entered Bus-Off at t=" << simTime() << "\n";
                discardQueuedFrames();
            }
        } else {
            rec = std::min(255, rec + 1);
            EV_INFO << topic << " REC=" << rec << " after observing active error on CAN ID "
                    << error->getCanID() << "\n";
        }
        delete error;
    }

    void handleSuccessfulFrame(CanDataFrame *frame) {
        if (frame->getKind() == ecuIndex) {
            tec = std::max(0, tec - 1);
            queuedCanIds.erase(frame->getCanID());
        } else {
            rec = std::max(0, rec - 1);
        }
        delete frame;
    }

  protected:
    int numInitStages() const override { return CanTrafficSourceAppBase::numInitStages(); }

    void initialize(int stage) override {
        CanTrafficSourceAppBase::initialize(stage);
        if (stage == 0) {
            topic = par("zmqTopic").stdstringValue();
            ecuIndex = par("ecuIndex").intValue();
            if (ecuIndex < 0 || ecuIndex > 2)
                throw cRuntimeError("ecuIndex must be 0, 1, or 2");
            pollTimer = new cMessage("zmqPoll");
            setupSocket();
        } else if (stage == 2) {
            scheduleAt(simTime() + par("pollInterval"), pollTimer);
        }
    }

    void handleMessage(cMessage *msg) override {
        if (msg == pollTimer) {
            pollPublisher();
            scheduleAt(simTime() + par("pollInterval"), pollTimer);
        } else if (msg->arrivedOn("errorIn")) {
            handleConfinementError(check_and_cast<ErrorFrame *>(msg));
        } else if (msg->arrivedOn("successIn")) {
            handleSuccessfulFrame(check_and_cast<CanDataFrame *>(msg));
        } else {
            CanTrafficSourceAppBase::handleMessage(msg);
        }
    }

    void finish() override {
        cancelAndDelete(pollTimer);
        pollTimer = nullptr;
        if (socket) {
            zmq_close(socket);
            socket = nullptr;
        }
        if (context) {
            zmq_ctx_term(context);
            context = nullptr;
        }
    }
};

Define_Module(ZmqEcuSourceApp);
