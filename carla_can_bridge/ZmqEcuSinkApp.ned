import fico4omnet.applications.ISinkApplication;

simple ZmqEcuSinkApp like ISinkApplication
{
    parameters:
        @class(ZmqEcuSinkApp);
        string outputEndpoint = default("tcp://127.0.0.1:5556");
    gates:
        input dataIn @directIn;
}
