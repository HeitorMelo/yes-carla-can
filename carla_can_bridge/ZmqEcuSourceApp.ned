import fico4omnet.applications.can.source.CanTrafficSourceAppBase;

simple ZmqEcuSourceApp extends CanTrafficSourceAppBase
{
    parameters:
        @class(ZmqEcuSourceApp);
        string zmqTopic;
        int ecuIndex;
        string inputEndpoint = default("tcp://127.0.0.1:5555");
        double pollInterval @unit(s) = default(0.0005s);
    gates:
        input errorIn @directIn;
        input successIn @directIn;
}
