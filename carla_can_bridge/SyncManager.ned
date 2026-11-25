simple SyncManager
{
    parameters:
        @class(SyncManager);
        string endpoint = default("tcp://127.0.0.1:5557");
        int socketTimeoutMs = default(30000);
}
