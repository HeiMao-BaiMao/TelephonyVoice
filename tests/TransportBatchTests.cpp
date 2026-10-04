#include "dsp/ChannelTransport.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#if TELEPHONY_USE_EVS_JBM
#include "dsp/SignalProcessor.h"
#endif

using namespace TelephonyDSP;
using namespace TelephonyDSP::Transport;
namespace {
int failures=0;
void check(bool pass,const char* message) {
    std::printf("%s: %s\n",pass?"ok":"FAIL",message); failures+=!pass;
}
void set(AdvancedSettings& s,AdvancedControl c,double value) {
    if(!s.setPlain(size_t(c),value)) check(false,"batch test control value is valid");
}
AdvancedSettings settings() {
    AdvancedSettings s;
    set(s,AdvancedControl::NetworkEnabled,1); set(s,AdvancedControl::PacketFormat,1);
    set(s,AdvancedControl::PlaybackDelayMs,100); set(s,AdvancedControl::BurstMean,1);
    return s;
}
CodecEncodedPacket packet(uint64_t sequence,double sourceMs,bool lost=false) {
    return {{CodecPacketFormat::G711MuLaw,8000,160},
            {uint8_t(sequence),uint8_t(sequence+1),uint8_t(sequence+2)},sequence,sourceMs,lost};
}
Bytes g192(int bits) {
    Bytes bytes(size_t(4+bits*2)); const uint16_t sync=0x6b21,count=uint16_t(bits);
    std::memcpy(bytes.data(),&sync,2); std::memcpy(bytes.data()+2,&count,2);
    for(int i=0;i<bits;++i) {
        const uint16_t bit=i%3?0x007f:0x0081; std::memcpy(bytes.data()+4+2*i,&bit,2);
    }
    return bytes;
}
void independentClocks() {
    ChannelTransport t; t.configure(settings(),0); std::vector<CodecPlayout> out;
    check(t.supportsBatchIngress(),"channel advertises asynchronous encoded batch ingress");
    t.exchangeBatch({packet(65535,0),packet(65536,20)},0,out);
    check(out.size()==2 && !t.warmingUp() && t.lastPacketReceived(),
          "two sender packets arrive immediately without a second fixed playout buffer");
    check(out.size()==2 && out[0].sequence==65535 && out[1].sequence==65536 &&
          out[1].sourceTimestampMs==20 && out[1].sentTimeMs==0 && out[1].arrivalTimeMs==0 &&
          out[1].playoutTimeMs==0 && out[1].hasTiming,
          "independent source timestamps and physical send/arrival deadlines survive RTP wrap");
    check(t.receiverReport().extendedHighestSequence==65536,
          "RTCP observes original wrapping sender wire sequences rather than receiver ticks");
    t.exchangeBatch({},1000,out);
    std::vector<RtcpPacket> sr,rr,xr;
    const bool reports=parseRtcp(t.senderReportBytes(),sr)&&parseRtcp(t.receiverReportBytes(),rr)&&
                       parseRtcp(t.extendedReportBytes(),xr);
    check(out.empty() && !t.lastPacketReceived() && t.telemetry().packets==2 &&
          t.telemetry().lost==0 && reports && sr[0].sender.packetCount==2 &&
          sr[0].sender.octetCount==6 && sr[0].sender.rtpTimestamp==160,
          "empty receiver ticks drain/report without inventing packets or advancing sender RTP time");
    check(reports && rr[0].reports[0].cumulativeLost==0 &&
          rr[0].reports[0].delaySinceLastSr==65536 && xr[0].xrBlocks.size()==1,
          "batch RTCP SR/RR/XR snapshots retain measured control timing");
    const uint64_t extended=(uint64_t(1)<<40)+9;
    t.exchangeBatch({packet(extended,1100)},1020,out);
    check(out.size()==1 && out[0].sequence==extended && out[0].arrivalTimeMs==1020 &&
          out[0].sourceTimestampMs==1100,
          "64-bit source sequence and media clock ahead of physical time remain unmodified");
    t.reset();
    check(t.pendingPackets()==0 && t.telemetry().packets==0 && !t.lastPacketReceived() &&
          t.senderReportBytes().empty(),"batch reset clears queues, last-arrival state and report snapshots");
}
void delayLossAndDuplicates() {
    auto s=settings(); set(s,AdvancedControl::LateProbability,1); set(s,AdvancedControl::LateDelayMs,100);
    ChannelTransport t; t.configure(s,0); std::vector<CodecPlayout> out;
    t.exchangeBatch({packet(0,0)},0,out);
    check(out.empty() && t.pendingPackets()==1 && t.telemetry().packets==1 && t.telemetry().lost==0,
          "pending network bytes are counted as submitted, never fabricated as playout loss");
    t.exchangeBatch({},99,out);
    check(out.empty() && t.pendingPackets()==1 && t.telemetry().lost==0,
          "empty batches leave future arrivals queued through the exact physical deadline");
    t.exchangeBatch({},100,out);
    check(out.size()==1 && out[0].sequence==0 && out[0].sentTimeMs==0 &&
          out[0].arrivalTimeMs==100 && out[0].playoutTimeMs==100 && t.pendingPackets()==0,
          "an empty sender batch still drains late encoded arrivals for the real JBM");
    s=settings(); set(s,AdvancedControl::DuplicateRate,1); t.configure(s,0); t.reset();
    t.exchangeBatch({packet(7,140)},0,out); const bool original=out.size()==1 && out[0].sequence==7;
    t.exchangeBatch({},20,out);
    check(original && out.size()==1 && out[0].sequence==7 && out[0].sourceTimestampMs==140 &&
          out[0].arrivalTimeMs==1 && t.telemetry().duplicates==1 && t.telemetry().lost==0,
          "actual duplicate arrivals keep their original source identity and reach the JBM");
    s=settings(); t.configure(s,0); t.reset();
    t.exchangeBatch({packet(0,0)},0,out);
    t.exchangeBatch({packet(1,20,true)},20,out);
    check(out.empty() && t.networkCounters().lost==1 && t.telemetry().packets==2 &&
          t.telemetry().lost==1 && t.telemetry().measuredLoss==0.5,
          "forced packet loss is measured from encoded submissions instead of receiver tick count");
    t.exchangeBatch({packet(2,40)},40,out); t.exchangeBatch({},1000,out);
    check(t.receiverReport().cumulativeLost==1 && t.receiverReport().extendedHighestSequence==2,
          "RTCP reports a real missing sender sequence after later packets arrive");
    t.reset(); t.configure(s,1); t.exchangeBatch({packet(0,0),packet(1,20)},0,out);
    check(out.empty() && t.networkCounters().lost==2 && t.telemetry().measuredLoss==1,
          "configured total loss drops every actual sender packet in a multi-frame batch");
}
void reorderAndVariableProduction() {
    auto s=settings(); set(s,AdvancedControl::JitterMs,80);
    ChannelTransport t; t.configure(s,0); std::vector<CodecPlayout> out;
    std::set<uint64_t> seen; uint64_t sequence=0; double lastArrival=-1;
    bool timings=true, ordered=true, multipleArrivals=false;
    for(int tick=0;tick<90;++tick) {
        std::vector<CodecEncodedPacket> encoded;
        // Synthetic independent sender clock: cycles of no frame, one frame,
        // and two frames, while receiver deadlines remain exactly 20 ms.
        for(int i=0;i<tick%3;++i) { encoded.push_back(packet(sequence,sequence*20.0)); ++sequence; }
        t.exchangeBatch(encoded,tick*20.0,out);
        multipleArrivals|=out.size()>1;
        for(const auto& arrival:out) {
            timings&=arrival.sourceTimestampMs==arrival.sequence*20.0 &&
                     arrival.arrivalTimeMs>=arrival.sentTimeMs && arrival.arrivalTimeMs<=tick*20;
            ordered&=arrival.arrivalTimeMs>=lastArrival; lastArrival=arrival.arrivalTimeMs;
            seen.insert(arrival.sequence);
        }
    }
    t.exchangeBatch({},2500,out);
    for(const auto& arrival:out) {
        ordered&=arrival.arrivalTimeMs>=lastArrival; lastArrival=arrival.arrivalTimeMs;
        seen.insert(arrival.sequence);
    }
    check(sequence==90 && seen.size()==sequence && t.telemetry().packets==sequence &&
          t.telemetry().lost==0 && t.pendingPackets()==0,
          "0/1/2 encoded frames per nominal tick all traverse the network queue without synthetic loss");
    check(timings && ordered && multipleArrivals && t.networkCounters().reordered>0,
          "real arrival reordering is drained in arrival order without relabeling source frames");
}
void redundancyAndLiveMetadata() {
    auto s=settings(); set(s,AdvancedControl::Redundancy,1);
    ChannelTransport t; t.configure(s,0); std::vector<CodecPlayout> out;
    t.exchangeBatch({packet(0,0)},0,out); t.exchangeBatch({packet(1,20,true)},20,out);
    t.exchangeBatch({packet(2,40)},40,out);
    check(out.size()==2 && out[0].sequence==1 && out[0].payload==packet(1,20).payload &&
          out[0].sourceTimestampMs==20 && out[0].arrivalTimeMs==40 && out[1].sequence==2 &&
          t.networkCounters().lost==1 && t.telemetry().lost==1,
          "RED reconstructs the missing original AU at its real successor arrival without erasing network loss");
    t.exchangeBatch({},1000,out);
    check(t.receiverReport().cumulativeLost==1,
          "RED reconstruction does not fabricate an original RTP packet in RTCP statistics");
    set(s,AdvancedControl::LateProbability,1); set(s,AdvancedControl::LateDelayMs,100);
    t.configure(s,0); t.reset(); t.exchangeBatch({packet(0,0)},0,out);
    set(s,AdvancedControl::LateProbability,0); t.configure(s,0);
    t.exchangeBatch({packet(1,20)},20,out);
    const bool recoveredEarly=out.size()==2 && out[0].sequence==0 && out[0].arrivalTimeMs==20;
    t.exchangeBatch({},100,out);
    check(recoveredEarly && out.size()==1 && out[0].sequence==0 && out[0].arrivalTimeMs==100 &&
          t.telemetry().lost==0,
          "RED early recovery never discards the later genuine original network arrival");
    s=settings(); set(s,AdvancedControl::LateProbability,1); set(s,AdvancedControl::LateDelayMs,100);
    t.configure(s,0); t.reset();
    CodecEncodedPacket amr{{CodecPacketFormat::AMRNB,8000,160},Bytes(32,0),10,0,false};
    amr.payload[0]=0x3c; amr.payload[1]=0x55;
    t.exchangeBatch({amr},0,out);
    set(s,AdvancedControl::AmrOctetAligned,0); t.configure(s,0);
    amr.sequence=11; amr.sourceTimestampMs=20; t.exchangeBatch({amr},20,out);
    set(s,AdvancedControl::PacketFormat,0); t.configure(s,0);
    amr.sequence=12; amr.sourceTimestampMs=40; t.exchangeBatch({amr},40,out);
    t.exchangeBatch({},140,out);
    check(out.size()==3 && out[0].payload==amr.payload && out[1].payload==amr.payload &&
          out[2].payload==amr.payload && out[0].sequence==10 && out[2].sequence==12,
          "batch packets retain octet-aligned, bandwidth-efficient and raw metadata across live changes");
    for(int style=0;style<=2;++style) {
        s=settings(); set(s,AdvancedControl::EvsPayloadStyle,style); t.configure(s,0); t.reset();
        CodecEncodedPacket evs{{CodecPacketFormat::EVSG192,48000,960},g192(264),23,1000,false};
        t.exchangeBatch({evs},0,out); std::vector<RtcpPacket> sr;
        check(out.size()==1 && out[0].payload==evs.payload && parseRtcp(t.senderReportBytes(),sr) &&
              sr[0].sender.rtpTimestamp==16000,
              "batch EVS compact/header-full serialization uses the independent 16 kHz source RTP clock");
    }
    s=settings(); set(s,AdvancedControl::EvsPayloadStyle,1); t.configure(s,0); t.reset();
    CodecEncodedPacket silence{{CodecPacketFormat::EVSG192,16000,320,true},g192(0),0,0,false};
    t.exchangeBatch({silence},0,out);
    check(out.size()==1 && out[0].payload==silence.payload && out[0].dtx,
          "EVS NO_DATA survives mandatory header-full framing and preserves DTX metadata");
}
void boundedAndInvalidInputs() {
    auto s=settings(); set(s,AdvancedControl::LateProbability,1); set(s,AdvancedControl::LateDelayMs,500);
    ChannelTransport t; t.configure(s,0); std::vector<CodecPlayout> out;
    std::vector<CodecEncodedPacket> burst;
    for(int i=0;i<1800;++i) burst.push_back(packet(i,i*20));
    t.exchangeBatch(burst,0,out);
    check(out.empty() && t.pendingPackets()==512 && t.networkCounters().overflow==1288 &&
          t.telemetry().packets==1800 && t.telemetry().lost==1288,
          "bounded batch network capacity records real rejected originals separately from pending bytes");
    t.exchangeBatch({},500,out);
    check(out.size()==512 && t.pendingPackets()==0,
          "all due arrivals retain metadata beyond bounded history and drain without a one-frame receive limit");
    s=settings(); set(s,AdvancedControl::DuplicateRate,1); t.reset(); t.configure(s,0);
    t.exchangeBatch(burst,0,out); const size_t originals=out.size();
    t.exchangeBatch({},20,out);
    check(originals==256 && out.size()==256 && t.telemetry().duplicates==256 && t.pendingPackets()==0,
          "pending duplicates retain original metadata when a large batch exceeds settled-history capacity");
    t.reset(); t.configure(settings(),0);
    auto invalid=packet(0,0); invalid.info.sampleRate=0;
    t.exchangeBatch({invalid},0,out);
    check(out.empty() && t.telemetry().packets==1 && t.telemetry().lost==1 &&
          t.networkCounters().submitted==0,
          "invalid timing is rejected as an adapter drop without claiming a network submission");
    t.exchangeBatch({packet(1,20)},std::numeric_limits<double>::quiet_NaN(),out);
    check(out.empty() && t.telemetry().packets==1,
          "nonfinite receiver deadlines neither enqueue packets nor mutate counters");
}
#if TELEPHONY_USE_EVS_JBM
void wholeSignalJbm() {
    constexpr int sampleRate=16000,frames=400;
    std::vector<float> input(sampleRate*8);
    for(size_t i=0;i<input.size();++i) {
        const double time=double(i)/sampleRate;
        const double envelope=0.8+0.2*std::sin(6.283185307179586*3.7*time);
        input[i]=float(envelope*(0.22*std::sin(6.283185307179586*317*time)+
                                0.14*std::sin(6.283185307179586*631*time)));
    }
    const auto run=[&](SignalProcessor& processor,int block) {
        std::vector<float> output(input.size());
        for(size_t offset=0;offset<input.size();offset+=block) {
            const int count=int(std::min<size_t>(block,input.size()-offset));
            float* source=input.data()+offset; float* destination=output.data()+offset;
            processor.process(&source,1,&destination,1,count);
        }
        return output;
    };
    for(double ppm:{0.0,50.0,-50.0}) {
        SignalProcessor processor;
        processor.setSampleRate(sampleRate); processor.setEVSConfig(sampleRate,13200,EVS_WB);
        processor.setMode(EraMode::EVS_JBM); processor.setParameters(1,0,false,0,0,0);
        auto s=settings(); set(s,AdvancedControl::ClockDriftPpm,ppm);
        set(s,AdvancedControl::PlaybackDelayMs,80); set(s,AdvancedControl::JitterMs,5);
        set(s,AdvancedControl::DuplicateRate,0.25); set(s,AdvancedControl::PureSilence,1);
        processor.setAdvancedSettings(s);
        const auto output=run(processor,511); const auto metrics=processor.getTelemetry();
        const uint64_t expected=uint64_t(std::floor(frames/(1+ppm*1e-6)));
        check(metrics.packets==expected && metrics.lost==0 && metrics.duplicates>0 && metrics.jitterMs>0,
              "whole SignalProcessor JBM carries independently produced frames through real RTP arrival batches");
        bool finite=true,continuous=true;
        for(float value:output) finite&=std::isfinite(value)&&std::abs(value)<2;
        for(size_t offset=sampleRate;offset+320<=output.size();offset+=320) {
            double energy=0; for(size_t i=offset;i<offset+320;++i) energy+=output[i]*output[i];
            continuous&=energy>1e-6;
        }
        check(finite && continuous && processor.getLatencySamples()==sampleRate*150/1000,
              "JBM speech remains audible after warmup without false DTX silence or a second fixed latency queue");
        processor.reset(); const auto repeated=run(processor,127);
        bool same=output.size()==repeated.size();
        for(size_t i=0;i<output.size();++i) same&=std::abs(output[i]-repeated[i])<1e-6;
        check(same && processor.getTelemetry().packets==expected,
              "whole JBM source production, network realization and audio repeat across reset and host block sizes");
    }
}
#endif
}
int main() {
    independentClocks(); delayLossAndDuplicates(); reorderAndVariableProduction();
    redundancyAndLiveMetadata(); boundedAndInvalidInputs();
#if TELEPHONY_USE_EVS_JBM
    wholeSignalJbm();
#endif
    return failures?1:0;
}
