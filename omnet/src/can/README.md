# CAN core library

This C++17 library is the protocol primitive layer used by OMNeT++ adapters. It
does not depend on OMNeT++, CARLA, ZeroMQ, or SocketCAN, so codec and controller
rules can be checked in a normal compiler environment.

## Public interface

- `CanFrame.h`: standard identifier, data frame, DLC 0–8, and payload bytes.
- `CanCodec.h`: `CanCodec::serialize(frame)` returns a `WireFrame` with every
  sampled bit, field, logical bit index, and a `stuffed` marker. Dominant is
  represented by `true`. `unstuffed_crc_input` is SOF through data, MSB first.
  `crc_start` and `crc_end` delimit the stuffed CRC sequence in `bits`.
- `CanCodec::decodeBusBits(samples, false)` validates a prefix ending at the
  CRC delimiter, so a controller can decide whether it may assert ACK. The
  default validates through intermission; pass `(samples, true, false)` to
  validate and deliver after EOF, then let the bus finish intermission.
- `CanController.h`: counter/state operations, transmit bit comparison, and
  arbitration-loss classification. Arbitration loss has no counter penalty;
  mismatch after arbitration is a bit error. The adapter reports detected
  errors and successful frames through the explicit counter methods. Error
  context arguments expose the documented ACK and flag exceptions. An
  error-passive transmitter reports an eight-bit suspend interval after a
  successful frame.
- `CanBus.h`: order-independent wired-AND resolution for all drivers at one
  sample point. A dominant value from any driver resolves dominant.
- `PassiveWireDecoder.h`: classifies already observed wire samples, including
  stuffing and field positions, without a transmit or intervention interface.

The earlier branch contains framework-free checks; those fixtures are not included in this branch.

## Deliberate limits

The codec rejects extended identifiers, remote frames, and DLC above eight.
It emits SOF through intermission for standard data frames. The shared bus
resolver models an ideal single sample point without propagation or oscillator
effects. The OMNeT++ adapter provides ideal simultaneous drive/resolve/sample
ordering, ACK receivers, and retries. Passive-only error flag continuation and
overlapping flag timing are not implemented; the former terminates the run
explicitly. The controller class provides counter and state rules; it does not
schedule those protocol actions.
This subset is not a claim of complete ISO 11898 conformance.

The implemented confinement profile uses TEC +8 on transmit errors, with the
passive ACK and arbitration stuff exceptions supplied as event context. REC
increments for receiver errors and supports the specified +8 flag conditions.
Successful transmission decrements TEC; successful reception decrements REC
below 128 and returns it to 119 above that threshold. Error passive begins at
128 and bus-off at TEC 256. The wrapper must apply the eight-bit suspend
interval, stop bus-off nodes from transmitting, ACKing, and signaling errors,
and classify which receivers detect each error event.
