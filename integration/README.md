# CAN-bus-simulator integration

The integration keeps the C ECU simulator and the CARLA/Python system as
separate processes. The default topology is:

```text
CAN-bus-simulator ECUs -- vcan1 -- can_ecu_adapter.py -- vcan0 -- CARLA system
```

The ownership registry is [`can_ecu_ownership.json`](can_ecu_ownership.json).
The adapter translates only these signals into the CARLA DBC:

| Simulator frame | Producer | CARLA message | Translation |
|---|---|---|---|
| `0x104` | `brake_ecu` | `BRAKE` | `0`/non-zero → `0`/`255` |
| `0x105` | `direction_ecu` | `GENERAL_LIGHTS` | `0=off`, `1=left`, `2=right`, `3=hazards` |
| `0x106` | `brake_ecu` | `THROTTLE` | `0`/non-zero → `0`/`255` |

Engine telemetry frames are recorded by the simulator but are not translated
because the current CARLA DBC has no corresponding actuation input.

## Run

Build the simulator from its directory:

```bash
cd CAN-bus-simulator
mkdir -p build
gcc -I include -o build/engine_ecu \
  src/engine_ECU/engine_ecu.c src/common/can.c src/common/logger.c \
  src/fault_injection/fault_injection.c
```

Build the other ECU binaries using the commands in its README. Create the
simulator bus:

```bash
CAN_INTERFACE=vcan1 ./scripts/setup_vcan.sh vcan1
```

Start the adapter from the project root in dry-run mode first:

```bash
python3 adapters/can_ecu_adapter.py \
  --source vcan1 \
  --target vcan0 \
  --dry-run
```

After validating the translations, remove `--dry-run` to transmit translated
frames to the CARLA bus. Keep the experiment on virtual interfaces only.
