simple AttackerNode
{
    parameters:
        @class(AttackerNode);
        int targetCanId = default(1537);
        bool attackActive = default(true);
        double bitrate @unit(bps) = default(500000bps);
        double injectionDelay @unit(s) = default(36us);
    gates:
        inout gate;
}
