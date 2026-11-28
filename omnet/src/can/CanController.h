#pragma once

#include "CanCodec.h"

namespace canlab {

enum class ControllerState { ErrorActive, ErrorPassive, BusOff };
enum class ErrorKind { Bit, Stuff, CRC, Form, Ack };
enum class TxObservation { Match, ArbitrationLost, BitError };

struct TransmitErrorContext {
    // True if a dominant bit was observed while the node sent its passive flag.
    bool dominantDuringPassiveErrorFlag = false;
    // Bosch fault-confinement exception for a specific arbitration stuff error.
    bool arbitrationStuffException = false;
};

struct ReceiveErrorContext {
    // A bit error while sending an active error or overload flag increments REC
    // by eight; it is not the ordinary +1 receiver error case.
    bool bitErrorDuringActiveErrorFlag = false;
    // Dominant as the first bit after an error flag adds eight to REC.
    bool dominantFirstBitAfterErrorFlag = false;
};

class CanController {
public:
    unsigned tec() const { return tec_; }
    unsigned rec() const { return rec_; }
    ControllerState state() const;
    bool mayTransmit() const { return state() != ControllerState::BusOff; }
    bool mayAcknowledge() const { return state() != ControllerState::BusOff; }
    bool emitsActiveErrorFlag() const { return state() == ControllerState::ErrorActive; }
    unsigned suspendTransmissionBits() const { return state() == ControllerState::ErrorPassive ? 8u : 0u; }

    TxObservation observeTransmitBit(const Bit& transmitted, bool busDominant) const;
    void transmitError(ErrorKind kind, TransmitErrorContext context = {});
    void receiveError(ErrorKind kind, ReceiveErrorContext context = {});
    void successfulTransmit();
    void successfulReceive();

private:
    unsigned tec_ = 0;
    unsigned rec_ = 0;
};

} // namespace canlab
