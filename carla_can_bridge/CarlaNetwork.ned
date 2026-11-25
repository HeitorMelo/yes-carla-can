import fico4omnet.bus.can.CanBus;
import fico4omnet.nodes.can.CanNode;

network CarlaNetwork
{
    parameters:
        double bitrate @unit(bps) = 500000bps;
    submodules:
        syncManager: SyncManager;
        canBus: CanBus {
            parameters:
                bandwidth = parent.bitrate;
                version = "2.0A";
                bitStuffingPercentage = 0.15;
            gates:
                gate[4];
        }
        powertrainEcu: CanNode {
            parameters:
                sourceApp[0].typename = "ZmqEcuSourceApp";
                sourceApp[0].zmqTopic = "Powertrain_ECU";
                sourceApp[0].ecuIndex = 0;
                sinkApp[0].typename = "ZmqEcuSinkApp";
                sinkApp[0].outputEndpoint = "tcp://127.0.0.1:5556";
        }
        chassisEcu: CanNode {
            parameters:
                sourceApp[0].typename = "ZmqEcuSourceApp";
                sourceApp[0].zmqTopic = "Chassis_ECU";
                sourceApp[0].ecuIndex = 1;
                sinkApp[0].typename = "ZmqEcuSinkApp";
                sinkApp[0].outputEndpoint = "tcp://127.0.0.1:5558";
        }
        adasEcu: CanNode {
            parameters:
                sourceApp[0].typename = "ZmqEcuSourceApp";
                sourceApp[0].zmqTopic = "ADAS_ECU";
                sourceApp[0].ecuIndex = 2;
                sinkApp[0].typename = "ZmqEcuSinkApp";
                sinkApp[0].outputEndpoint = "tcp://127.0.0.1:5560";
        }
        attacker: AttackerNode {
            parameters:
                targetCanId = 1537;
                bitrate = parent.bitrate;
                injectionDelay = 36us;
        }
    connections:
        powertrainEcu.gate <--> canBus.gate[0];
        chassisEcu.gate <--> canBus.gate[1];
        adasEcu.gate <--> canBus.gate[2];
        attacker.gate <--> canBus.gate[3];
}
