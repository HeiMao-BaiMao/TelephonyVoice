#include "dsp/ChannelTransport.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace TelephonyDSP;
using namespace TelephonyDSP::Transport;
namespace {
int failures=0;
void check(bool pass,const char* message) { std::printf("%s: %s\n",pass?"ok":"FAIL",message); failures+=!pass; }
void set(AdvancedSettings& settings,AdvancedControl control,double value) { settings.setPlain(size_t(control),value); }
AdvancedSettings settings() {
    AdvancedSettings s;set(s,AdvancedControl::NetworkEnabled,1);set(s,AdvancedControl::PacketFormat,1);
    set(s,AdvancedControl::PlaybackDelayMs,20);return s;
}
const CodecPacketInfo pcm{CodecPacketFormat::G711MuLaw,8000,160};
Bytes input(int index) {return {uint8_t(index),uint8_t(index+1),uint8_t(index+2)};}
bool exchange(ChannelTransport& transport,int index,bool lost,CodecPlayout& out,const CodecPacketInfo& info=pcm) {
    const auto bytes=input(index);return transport.exchange(info,bytes.data(),bytes.size(),lost,out);
}
Bytes g192(int bits) {
    Bytes b(size_t(4+bits*2));const uint16_t sync=0x6b21,n=uint16_t(bits);
    std::memcpy(b.data(),&sync,2);std::memcpy(b.data()+2,&n,2);
    for(int i=0;i<bits;++i) {const uint16_t bit=i%3?0x007f:0x0081;std::memcpy(b.data()+4+i*2,&bit,2);}
    return b;
}
void queuesAndRecovery() {
    ChannelTransport t;auto s=settings();t.configure(s,0);CodecPlayout out;
    check(!exchange(t,0,false,out)&&t.warmingUp(),"encoded transport prebuffers exactly configured frame delay");
    check(exchange(t,1,false,out)&&!t.warmingUp()&&out.payload==input(0)&&out.nextPayload==input(1),"encoded RTP playout returns current and future FEC payloads");
    check(exchange(t,2,false,out)&&out.payload==input(1),"FEC look-ahead remains queued for its normal playout");
    t.reset();exchange(t,0,false,out);exchange(t,1,true,out);
    check(!exchange(t,2,false,out)&&out.nextPayload==input(2),"isolated loss exposes next packet for Opus decode_fec");
    check(exchange(t,3,false,out)&&out.payload==input(2),"normal decode follows FEC look-ahead without consuming it twice");
    set(s,AdvancedControl::Redundancy,1);t.configure(s,0);t.reset();
    exchange(t,0,false,out);exchange(t,1,true,out);
    check(exchange(t,2,false,out)&&out.payload==input(1),"RFC2198 recovers explicitly dropped encoded packet from successor");
    check(exchange(t,3,false,out)&&out.payload==input(2)&&t.networkCounters().lost==1,"RED recovery preserves next primary and real network loss counters");
    t.reset();exchange(t,0,false,out);exchange(t,1,true,out);
    check(!exchange(t,2,true,out),"single-generation RED does not invent recovery during consecutive losses");
    set(s,AdvancedControl::Redundancy,0);set(s,AdvancedControl::DuplicateRate,1);t.configure(s,0);t.reset();
    for(int i=0;i<20;++i) exchange(t,i,false,out);
    check(t.telemetry().duplicates>0&&t.telemetry().lost==0,"duplicate wire packets are discarded without audible false loss");
    set(s,AdvancedControl::DuplicateRate,0);set(s,AdvancedControl::LateProbability,1);set(s,AdvancedControl::LateDelayMs,100);t.configure(s,0);t.reset();
    for(int i=0;i<20;++i) exchange(t,i,false,out);
    check(t.telemetry().lost==19&&t.telemetry().late>0,"late arrival deadline causes measured playout loss");
}
void packetModesAndReports() {
    ChannelTransport t;auto s=settings();t.configure(s,0);CodecPlayout out;
    for(int i=0;i<110;++i) exchange(t,i,false,out);
    std::vector<RtcpPacket> sr,rr,xr;
    check(parseRtcp(t.senderReportBytes(),sr)&&parseRtcp(t.receiverReportBytes(),rr)&&parseRtcp(t.extendedReportBytes(),xr),
          "encoded transport exposes parseable RTCP SR/RR/XR snapshots");
    check(sr[0].sender.packetCount==102&&sr[0].sender.octetCount==306&&sr[0].sender.rtpTimestamp==16160,
          "RTCP sender packet/octet/timestamp counters reflect encoded payload traffic");
    check(rr[0].reports[0].cumulativeLost==0&&rr[0].reports[0].lastSr!=0&&rr[0].reports[0].delaySinceLastSr==65536,
          "RTCP receiver reports consume prior sender report and measured elapsed delay");
    check(xr[0].xrBlocks.size()==1&&xr[0].xrBlocks[0].type==4&&t.receiverReport().extendedHighestSequence==101,
          "RTCP XR RRTR and public receiver statistics are live");
    t.reset();check(t.senderReportBytes().empty()&&t.receiverReportBytes().empty()&&t.networkCounters().submitted==0,"transport reset clears report snapshots and counters");
    // Decode queued frames according to the representation at enqueue time.
    CodecPacketInfo nb{CodecPacketFormat::AMRNB,8000,160};
    Bytes amr{0x3c};amr.resize(32,0);amr[1]=0x55;
    t.configure(s,0);t.exchange(nb,amr.data(),amr.size(),false,out);
    set(s,AdvancedControl::AmrOctetAligned,0);t.configure(s,0);
    check(t.exchange(nb,amr.data(),amr.size(),false,out)&&out.payload==amr,"live AMR alignment change preserves queued old-format frames");
    set(s,AdvancedControl::PacketFormat,0);t.configure(s,0);
    check(t.exchange(nb,amr.data(),amr.size(),false,out)&&out.payload==amr,"live RTP toggle preserves queued packet metadata");
    check(t.exchange(nb,amr.data(),amr.size(),false,out)&&out.payload==amr,"non-RTP internal packet path still delivers native encoded bytes");
    s=settings();set(s,AdvancedControl::PlaybackDelayMs,0);t.configure(s,0);t.reset();
    CodecPacketInfo opus{CodecPacketFormat::Opus,16000,320};
    for(int i=0;i<51;++i) exchange(t,i,false,out,opus);
    parseRtcp(t.senderReportBytes(),sr);check(sr[0].sender.rtpTimestamp==48000,"Opus RTP clock is always 48k independent of PCM rate");
    CodecPacketInfo evs{CodecPacketFormat::EVSG192,48000,960};const auto speech=g192(264);
    for(int style=0;style<=2;++style){
        set(s,AdvancedControl::EvsPayloadStyle,style);t.configure(s,0);t.reset();bool okay=true;
        for(int i=0;i<51;++i)okay&=t.exchange(evs,speech.data(),speech.size(),false,out)&&out.payload==speech;
        parseRtcp(t.senderReportBytes(),sr);
        check(okay&&sr[0].sender.rtpTimestamp==16000,"EVS G.192/compact/header-full transport uses 16k RTP clock");
    }
    auto noData=g192(0);set(s,AdvancedControl::EvsPayloadStyle,1);t.configure(s,0);t.reset();
    check(t.exchange(evs,noData.data(),noData.size(),false,out)&&out.payload==noData,"compact-preferred EVS preserves mandatory header-full NO_DATA");
    set(s,AdvancedControl::HostClock,1);t.configure(s,0);t.reset();t.setTime(-1,true);
    t.exchange(evs,speech.data(),speech.size(),false,out);parseRtcp(t.senderReportBytes(),sr);
    check(sr[0].sender.rtpTimestamp==uint32_t(-16000)&&t.receiverReport().extendedHighestSequence==0,
          "negative host pre-roll maps to wrapped RTP timestamp without invalid conversion");
    CodecPacketInfo invalid{CodecPacketFormat::G711MuLaw,0,160};
    check(!t.exchange(invalid,nullptr,0,false,out)&&!t.warmingUp(),"invalid codec timing is rejected safely");
}
}
int main(){queuesAndRecovery();packetModesAndReports();return failures?1:0;}
