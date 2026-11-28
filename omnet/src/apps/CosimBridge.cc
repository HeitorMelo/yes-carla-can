#include <omnetpp.h>
#include <zmq.h>
#include "CanEvent_m.h"
#include <sstream>
#include <iomanip>
#include <string>
#include <climits>
using namespace omnetpp;
namespace canlab {
class CosimBridge : public cSimpleModule {
    void *ctx=nullptr, *socket=nullptr;
    cMessage *barrier=nullptr;
    std::ostringstream deliveries;
    unsigned long sequence=0;
    void reply(const std::string& s) { if (zmq_send(socket,s.data(),s.size(),0)<0) throw cRuntimeError("ZMQ send failed"); }
    void receiveStep() {
        char buf[65536]; int n=zmq_recv(socket,buf,sizeof(buf),0);
        if(n<0 || n>=static_cast<int>(sizeof(buf))) throw cRuntimeError("ZMQ receive failed or request too large");
        std::istringstream in(std::string(buf,n)); std::string line; std::getline(in,line);
        if(line=="STOP") { reply("BYE"); endSimulation(); return; }
        std::istringstream header(line); std::string cmd; long long target; header>>cmd>>target;
        if(cmd!="STEP" || !header || SimTime(target,SIMTIME_NS)<=simTime()) throw cRuntimeError("Invalid step header");
        while(std::getline(in,line)) {
            if(line.empty()) continue;
            std::istringstream frame(line); std::string ecu,hex; int id; frame>>ecu>>id>>hex;
            if(!frame || hex.size()%2 || hex.size()>16 || id<0 || id>2047) throw cRuntimeError("Invalid frame");
            if(ecu!="Powertrain_ECU" && ecu!="Chassis_ECU" && ecu!="ADAS_ECU" && ecu!="Body_ECU") throw cRuntimeError("Invalid ECU");
            auto *e=new CanEvent("cosimTx"); e->setEventType("tx_request"); e->setEcu(ecu.c_str());
            auto uid=ecu+":"+std::to_string(sequence++); e->setFrameUid(uid.c_str()); e->setCanId(id); e->setDlc(hex.size()/2);
            for(size_t i=0;i<hex.size()/2;i++) { size_t used=0; unsigned long value; try { value=std::stoul(hex.substr(i*2,2),&used,16); } catch(...) { delete e; throw cRuntimeError("Invalid hex"); } if(used!=2) { delete e; throw cRuntimeError("Invalid hex"); } e->setData(i,value); }
            send(e,"tx");
        }
        scheduleAt(SimTime(target,SIMTIME_NS),barrier);
    }
protected:
    void initialize() override { ctx=zmq_ctx_new(); socket=zmq_socket(ctx,ZMQ_REP); int linger=0; zmq_setsockopt(socket,ZMQ_LINGER,&linger,sizeof(linger)); if(zmq_bind(socket,par("endpoint").stringValue())) throw cRuntimeError("Cannot bind ZMQ endpoint"); barrier=new cMessage("step"); barrier->setSchedulingPriority(SHRT_MAX); scheduleAt(SIMTIME_ZERO,barrier); }
    void handleMessage(cMessage *m) override {
        if(m==barrier) { if(simTime()!=SIMTIME_ZERO) { reply("OK\n"+deliveries.str()); deliveries.str(""); deliveries.clear(); } receiveStep(); return; }
        auto *e=check_and_cast<CanEvent*>(m); if(std::string(e->getEventType())!="rx_delivery") throw cRuntimeError("Expected delivery");
        deliveries<<e->getCanId()<<' ';
        for(int i=0;i<e->getDlc();++i) deliveries<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned>(e->getData(i));
        deliveries<<std::dec<<'\n'; delete e;
    }
    void finish() override { cancelAndDelete(barrier); if(socket) zmq_close(socket); if(ctx) zmq_ctx_term(ctx); }
};
Define_Module(CosimBridge);
}
