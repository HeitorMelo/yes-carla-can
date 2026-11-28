package canlab;
simple CosimBridge {
    parameters:
        string endpoint = default("tcp://127.0.0.1:5557");
        @class(canlab::CosimBridge);
    gates:
        output tx;
        input rx;
}
network CosimNetwork {
    submodules:
        bridge: CosimBridge;
        bus: CanBusModule {
            parameters:
                receiverNames = "ActuatorGateway";
        }
        observer: PassiveObserver;
    connections allowunconnected:
        bridge.tx --> bus.txIn++;
        bus.rxOut++ --> bridge.rx;
        bus.bitTap --> observer.observe;
}
