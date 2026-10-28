#include "CanController.h"
#include <algorithm>

namespace canlab {

ControllerState CanController::state() const {
    if (tec_ > 255) return ControllerState::BusOff;
    if (tec_ > 127 || rec_ > 127) return ControllerState::ErrorPassive;
    return ControllerState::ErrorActive;
}

TxObservation CanController::observeTransmitBit(const Bit& transmitted, bool busDominant) const {
    if (transmitted.dominant || !busDominant) return TxObservation::Match;
    if (transmitted.field == Field::Arbitration) return TxObservation::ArbitrationLost;
    // ACK slot is recessive by the transmitter; dominant here is a successful ACK.
    if (transmitted.field == Field::ACK) return TxObservation::Match;
    return TxObservation::BitError;
}

void CanController::transmitError(ErrorKind kind, TransmitErrorContext context) {
    if (state() == ControllerState::BusOff) return;
    if (kind == ErrorKind::Stuff && context.arbitrationStuffException) return;
    // Bosch exception: an error-passive transmitter does not increase TEC for
    // an ACK error only when its passive error flag remains recessive.
    if (kind == ErrorKind::Ack && state() == ControllerState::ErrorPassive &&
        !context.dominantDuringPassiveErrorFlag) return;
    tec_ = std::min(256u, tec_ + 8u);
}

void CanController::receiveError(ErrorKind kind, ReceiveErrorContext context) {
    if (state() == ControllerState::BusOff) return;
    if (context.dominantFirstBitAfterErrorFlag) {
        rec_ = std::min(255u, rec_ + 8u);
    } else if (context.bitErrorDuringActiveErrorFlag && kind == ErrorKind::Bit) {
        rec_ = std::min(255u, rec_ + 8u);
    } else {
        rec_ = std::min(255u, rec_ + 1u);
    }
}

void CanController::successfulTransmit() {
    if (state() == ControllerState::BusOff) return;
    if (tec_ > 0) --tec_;
}

void CanController::successfulReceive() {
    if (state() == ControllerState::BusOff) return;
    if (rec_ > 127) rec_ = 119;
    else if (rec_ > 0) --rec_;
}

} // namespace canlab
