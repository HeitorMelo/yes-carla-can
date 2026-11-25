#include <omnetpp.h>

#include "fico4omnet/linklayer/can/messages/CanDataFrame_m.h"
#include "fico4omnet/linklayer/can/messages/ErrorFrame_m.h"

using namespace omnetpp;
using namespace FiCo4OMNeT;

class AttackerNode : public cSimpleModule {
  private:
    cMessage *injectTimer = nullptr;
    cMessage *frameEndTimer = nullptr;
    unsigned int targetCanId = 0;
    unsigned int activeCanId = 0;
    int activeSource = -1;
    int pendingOrigin = -1;
    int pendingCanId = -1;
    int pendingBitLength = 0;
    bool attackActive = false;
    bool frameActive = false;

    cModule *sourceApp(int index) {
        static const char *nodeNames[] = {"powertrainEcu", "chassisEcu", "adasEcu"};
        if (index < 0 || index >= 3)
            return nullptr;
        cModule *node = getParentModule()->getSubmodule(nodeNames[index]);
        return node ? node->getSubmodule("sourceApp", 0) : nullptr;
    }

    void notifyEcusOfError(const ErrorFrame *error) {
        for (int i = 0; i < 3; ++i) {
            cModule *app = sourceApp(i);
            if (!app)
                continue;
            ErrorFrame *notice = error->dup();
            sendDirect(notice, app, "errorIn");
        }
    }

    void notifyEcusOfSuccess(const CanDataFrame *frame) {
        for (int i = 0; i < 3; ++i) {
            cModule *app = sourceApp(i);
            if (!app)
                continue;
            CanDataFrame *notice = frame->dup();
            sendDirect(notice, app, "successIn");
        }
    }

    void abortPendingFrame() {
        if (frameEndTimer->isScheduled())
            cancelEvent(frameEndTimer);
        frameActive = false;
    }

  protected:
    void initialize() override {
        targetCanId = par("targetCanId").intValue();
        attackActive = par("attackActive").boolValue();
        injectTimer = new cMessage("injectActiveErrorFlag");
        frameEndTimer = new cMessage("frameTransmissionComplete");
    }

    void handleMessage(cMessage *msg) override {
        if (msg == injectTimer) {
            if (!frameActive || pendingOrigin < 0) {
                return;
            }

            ErrorFrame *error = new ErrorFrame("activeErrorFlag");
            error->setCanID(static_cast<unsigned int>(pendingCanId));
            error->setNode(pendingOrigin);
            error->setKind(0); // FiCo4OMNeT: 0 is a transmitter bit error.
            error->setPos(18);
            notifyEcusOfError(error);
            abortPendingFrame();
            send(error, "gate$o");
        } else if (msg == frameEndTimer) {
            if (frameActive) {
                CanDataFrame *success = new CanDataFrame("successfulCanFrame");
                success->setCanID(activeCanId);
                success->setKind(activeSource);
                notifyEcusOfSuccess(success);
                delete success;
                frameActive = false;
            }
        } else if (auto *frame = dynamic_cast<CanDataFrame *>(msg)) {
            // FiCo broadcasts the frame when transmission begins. Keep a timer
            // until the nominal frame duration so errors can cancel delivery.
            if (!frameActive) {
                activeCanId = frame->getCanID();
                activeSource = frame->getKind();
                pendingCanId = static_cast<int>(frame->getCanID());
                pendingOrigin = frame->getKind();
                pendingBitLength = static_cast<int>(frame->getBitLength());
                frameActive = true;
                const double bitrate = par("bitrate").doubleValue();
                const simtime_t duration = SimTime(static_cast<double>(pendingBitLength) / bitrate);
                scheduleAt(simTime() + duration, frameEndTimer);
                if (attackActive && frame->getCanID() == targetCanId && pendingOrigin >= 0) {
                    scheduleAt(simTime() + par("injectionDelay"), injectTimer);
                }
            }
            delete frame;
        } else if (auto *error = dynamic_cast<ErrorFrame *>(msg)) {
            if (frameActive && error->getCanID() == activeCanId)
                abortPendingFrame();
            delete error;
        } else {
            delete msg;
        }
    }

    void finish() override {
        cancelAndDelete(injectTimer);
        cancelAndDelete(frameEndTimer);
        injectTimer = nullptr;
        frameEndTimer = nullptr;
    }
};

Define_Module(AttackerNode);
