#include <omnetpp.h>
#include <zmq.h>

#include "fico4omnet/linklayer/can/messages/CanDataFrame_m.h"

#include <cerrno>
#include <string>

using namespace omnetpp;
using namespace FiCo4OMNeT;

namespace {
const char *ecuTopic(int index) {
    static const char *topics[] = {"Powertrain_ECU", "Chassis_ECU", "ADAS_ECU"};
    return index >= 0 && index < 3 ? topics[index] : "Unknown_ECU";
}
}

class ZmqEcuSinkApp : public cSimpleModule {
  private:
    void *context = nullptr;
    void *socket = nullptr;

  protected:
    void initialize() override {
        context = zmq_ctx_new();
        if (!context)
            throw cRuntimeError("Cannot create ZeroMQ context: %s", zmq_strerror(errno));
        socket = zmq_socket(context, ZMQ_PUB);
        if (!socket)
            throw cRuntimeError("Cannot create ZeroMQ PUB socket: %s", zmq_strerror(errno));

        const int linger = 0;
        zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
        const char *endpoint = par("outputEndpoint").stringValue();
        if (zmq_bind(socket, endpoint) != 0)
            throw cRuntimeError("Cannot bind CAN output publisher at %s: %s",
                                endpoint, zmq_strerror(errno));
    }

    void handleMessage(cMessage *msg) override {
        CanDataFrame *frame = check_and_cast<CanDataFrame *>(msg);
        std::string payloadHex;
        if (frame->getEncapsulatedPacket())
            payloadHex = frame->getEncapsulatedPacket()->getName();

        const std::string wireMessage = std::string(ecuTopic(frame->getKind())) + " " +
                                        std::to_string(frame->getCanID()) + " " + payloadHex;
        EV_INFO << getFullPath() << " publishing " << wireMessage << "\n";
        const int sent = zmq_send(socket, wireMessage.data(), wireMessage.size(), ZMQ_DONTWAIT);
        if (sent < 0 && errno != EAGAIN)
            EV_WARN << "Could not publish surviving CAN frame: " << zmq_strerror(errno) << "\n";
        delete frame;
    }

    void finish() override {
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

Define_Module(ZmqEcuSinkApp);
