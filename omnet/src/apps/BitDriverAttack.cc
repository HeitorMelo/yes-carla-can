#include "BitDriverAttack.h"
#include "CanEvent_m.h"

#include <string>

using namespace omnetpp;

namespace canlab {
Define_Module(BitDriverAttack);

void BitDriverAttack::initialize()
{
    targetCanId = par("targetCanId").intValue();
    targetLogicalBit = par("targetLogicalBit").intValue();
    maxInterventions = par("maxInterventions").intValue();
    startAt = SimTime(par("startAt"));
    stopAt = SimTime(par("stopAt"));
    if (targetCanId < 0 || targetCanId > 0x7ff || targetLogicalBit < 0 || maxInterventions < 0 || stopAt < startAt)
        throw cRuntimeError("invalid bit-driver target or active interval");
}

void BitDriverAttack::handleMessage(cMessage *message)
{
    auto *observation = check_and_cast<CanEvent *>(message);
    const std::string type = observation->getEventType();
    if (type == "wire_idle") observer.idle();
    if (type == "wire_sample") observer.sample(observation->getDominant());
    const bool active = simTime() >= startAt && simTime() < stopAt;
    const bool target = type == "wire_sample" && active &&
        observer.nextDataBitIs(static_cast<std::uint16_t>(targetCanId), targetLogicalBit);
    if (target && interventions < maxInterventions) {
        auto *drive = new CanEvent("maliciousBitDrive");
        drive->setEventType("attack_drive");
        drive->setDominant(true);
        send(drive, "drive");
        ++interventions;
    }
    delete observation;
}

} // namespace canlab
