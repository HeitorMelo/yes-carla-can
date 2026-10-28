// Internal OMNeT++ control/trace packet. `ecu` and `frameUid` are simulator
// bookkeeping; CAN receivers must not treat them as authenticated wire data.
namespace canlab;

packet CanEvent
{
    string eventType;
    string ecu;
    string frameUid;
    int attempt = 0;
    int canId = 0;
    int dlc = 0;
    unsigned char data[8];
    int bitIndex = -1;
    string bitField;
    bool dominant = false;
    int tec = 0;
    int rec = 0;
    string controllerState;
    int queueDepth = 0;
}
