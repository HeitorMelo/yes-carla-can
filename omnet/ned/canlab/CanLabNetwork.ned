package canlab;

simple SyntheticEcuApp
{
    parameters:
        string ecuName;
        string frameSpecs;
        int maxReleases = default(-1);
        int releasePriority = default(0);
        @class(canlab::SyntheticEcuApp);
        @signal[applicationRelease](long);
        @signal[applicationReceive](long);
    gates:
        input rx;
        output tx;
}

simple ActuatorGateway
{
    parameters:
        double freshnessTimeout @unit(s) = default(150ms);
        string freshnessFile = default("freshness.jsonl");
        @class(canlab::ActuatorGateway);
        @signal[actuatorUpdate](long);
    gates:
        input rx;
}

simple PassiveObserver
{
    parameters:
        bool enabled = default(true);
        string outputFile = default("wire-observations.jsonl");
        @class(canlab::PassiveObserver);
    gates:
        input observe;
}

simple BitDriverAttack
{
    parameters:
        int targetCanId = default(0x601);
        int targetLogicalBit = default(19);
        int maxInterventions = default(32);
        double startAt @unit(s) = default(0s);
        double stopAt @unit(s) = default(100s);
        @class(canlab::BitDriverAttack);
    gates:
        input observe;
        output drive;
}

simple CanBusModule
{
    parameters:
        string ecuNames = default("Powertrain_ECU,Chassis_ECU,ADAS_ECU,Body_ECU,ActuatorGateway");
        string receiverNames = default("Powertrain_ECU,Chassis_ECU,ADAS_ECU,Body_ECU,ActuatorGateway");
        int experimentSeed = default(1);
        double bitDuration @unit(s) = default(2us);
        bool ackEnabled = default(true);
        int maxQueueDepth = default(256);
        string traceFile = default("");
        @class(canlab::CanBusModule);
    gates:
        input txIn[];
        input attackIn;
        output rxOut[];
        output bitTap;
}

network CanLabNetwork
{
    submodules:
        powertrain: SyntheticEcuApp {
            parameters:
                ecuName = "Powertrain_ECU";
                frameSpecs = "0x600,100000000,0,4,00000000;0x603,200000000,0,1,00;0x606,500000000,0,1,00;0x607,200000000,0,4,00000000";
        }
        chassis: SyntheticEcuApp {
            parameters:
                ecuName = "Chassis_ECU";
                frameSpecs = "0x601,100000000,0,4,80000000;0x602,100000000,1000000,4,00000000;0x604,200000000,0,1,00";
        }
        adas: SyntheticEcuApp {
            parameters:
                ecuName = "ADAS_ECU";
                frameSpecs = "0x605,500000000,0,4,00000000;0x60e,1000000000,0,8,0000000000000000;0x60f,0,0,2,0000;0x610,0,0,2,0000;0x611,100000000,1000000,6,000000000000;0x612,100000000,2000000,6,000000000000;0x613,100000000,3000000,2,0000;0x614,0,4000000,8,0000000000000000";
        }
        body: SyntheticEcuApp {
            parameters:
                ecuName = "Body_ECU";
                frameSpecs = "0x60c,0,0,1,00;0x60d,0,0,2,0000";
        }
        actuator: ActuatorGateway;
        observer: PassiveObserver;
        bus: CanBusModule;
    connections allowunconnected:
        powertrain.tx --> bus.txIn++;
        chassis.tx --> bus.txIn++;
        adas.tx --> bus.txIn++;
        body.tx --> bus.txIn++;
        bus.rxOut++ --> powertrain.rx;
        bus.rxOut++ --> chassis.rx;
        bus.rxOut++ --> adas.rx;
        bus.rxOut++ --> body.rx;
        bus.rxOut++ --> actuator.rx;
        bus.bitTap --> observer.observe;
}
