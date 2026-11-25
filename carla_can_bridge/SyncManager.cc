#include <omnetpp.h>
#include <zmq.h>

#include <cerrno>
#include <climits>
#include <cstring>
#include <string>

using namespace omnetpp;

class SyncManager : public cSimpleModule {
  private:
    void *context = nullptr;
    void *socket = nullptr;
    cMessage *tickTimer = nullptr;
    bool stepReplyPending = false;

    std::string receiveRequest() {
        char buffer[256];
        const int length = zmq_recv(socket, buffer, sizeof(buffer), 0);
        if (length < 0)
            throw cRuntimeError("ZeroMQ clock receive failed: %s", zmq_strerror(errno));
        return std::string(buffer, static_cast<size_t>(length));
    }

    void reply(const char *value) {
        if (zmq_send(socket, value, std::strlen(value), 0) < 0)
            throw cRuntimeError("ZeroMQ clock reply failed: %s", zmq_strerror(errno));
    }

    bool armNextStep(const std::string& request) {
        if (request == "STOP") {
            reply("BYE");
            endSimulation();
            return false;
        }

        double target = 0.0;
        try {
            target = std::stod(request);
        } catch (const std::exception&) {
            throw cRuntimeError("Invalid co-simulation time request: '%s'", request.c_str());
        }
        if (target <= simTime().dbl())
            throw cRuntimeError("Requested time %.9f must be after current OMNeT++ time %.9f",
                                target, simTime().dbl());

        scheduleAt(SimTime(target), tickTimer);
        stepReplyPending = true;
        return true;
    }

  protected:
    void initialize() override {
        context = zmq_ctx_new();
        if (!context)
            throw cRuntimeError("Cannot create ZeroMQ context: %s", zmq_strerror(errno));
        socket = zmq_socket(context, ZMQ_REP);
        if (!socket)
            throw cRuntimeError("Cannot create ZeroMQ REP socket: %s", zmq_strerror(errno));

        const int linger = 0;
        const int timeoutMs = par("socketTimeoutMs").intValue();
        zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
        zmq_setsockopt(socket, ZMQ_RCVTIMEO, &timeoutMs, sizeof(timeoutMs));
        const char *endpoint = par("endpoint").stringValue();
        if (zmq_bind(socket, endpoint) != 0)
            throw cRuntimeError("Cannot bind clock socket at %s: %s", endpoint, zmq_strerror(errno));

        tickTimer = new cMessage("lockstepTick");
        // Process normal events at the boundary before acknowledging the step.
        tickTimer->setSchedulingPriority(SHRT_MAX);
        scheduleAt(simTime(), tickTimer);
    }

    void handleMessage(cMessage *msg) override {
        if (msg != tickTimer)
            throw cRuntimeError("Unexpected message at SyncManager");

        // Reply only at the target event time, then wait for CARLA to finish
        // its physics tick and request the next simulation window.
        if (stepReplyPending) {
            reply("OK");
            stepReplyPending = false;
        }

        std::string request = receiveRequest();
        if (request == "READY") {
            reply("READY");
            request = receiveRequest();
        }
        armNextStep(request);
    }

    void finish() override {
        cancelAndDelete(tickTimer);
        tickTimer = nullptr;
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

Define_Module(SyncManager);
