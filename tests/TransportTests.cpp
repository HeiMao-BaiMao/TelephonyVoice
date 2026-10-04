#include "dsp/Transport.h"
#include "dsp/NetworkProfile.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>

using namespace TelephonyDSP;
using namespace TelephonyDSP::Transport;
namespace {
int failures = 0;
void check(bool pass, const char* name) { std::printf("%s: %s\n", pass ? "ok" : "FAIL", name); failures += !pass; }
void rtpTests() {
    RtpPacket p; p.marker = true; p.payloadType = 111; p.sequence = 0xffff;
    p.timestamp = 0x01020304; p.ssrc = 0xaabbccdd; p.payload = {0x55, 0xaa};
    Bytes encoded;
    const Bytes expected{0x80,0xef,0xff,0xff,1,2,3,4,0xaa,0xbb,0xcc,0xdd,0x55,0xaa};
    check(serializeRtp(p, encoded) && encoded == expected, "RTP fixed header network-order known vector");
    p.csrcs = {0x11223344,0x55667788}; p.hasExtension = true; p.extensionProfile = 0xbede;
    p.extension = {0x10, 0x11, 0, 0}; p.padding = 4;
    RtpPacket decoded;
    check(serializeRtp(p, encoded) && parseRtp(encoded, decoded) && decoded.sequence == p.sequence && decoded.csrcs == p.csrcs &&
          decoded.extension == p.extension && decoded.extensionProfile == p.extensionProfile && decoded.payload == p.payload && decoded.padding == 4,
          "RTP CSRC/extension/padding round-trip");
    auto bad = encoded; bad[0] = 0; check(!parseRtp(bad, decoded), "RTP rejects invalid version");
    bad = encoded; bad.back() = 0; check(!parseRtp(bad, decoded), "RTP rejects zero padding count");
    bad = encoded; bad.back() = 200; check(!parseRtp(bad, decoded), "RTP rejects excessive padding");
    bad = encoded; bad[22] = 0xff; bad[23] = 0xff; check(!parseRtp(bad, decoded), "RTP rejects truncated extension");
    bool truncated = true;
    for (std::size_t i = 0; i < 12; ++i) truncated &= !parseRtp(Bytes(expected.begin(),expected.begin()+i), decoded);
    check(truncated, "RTP rejects every fixed-header truncation");
    check(sequenceBefore(65535, 0) && !sequenceBefore(0,65535) && !sequenceBefore(12,12), "RTP sequence comparison wraps correctly");
    p.payloadType = 128; check(!serializeRtp(p, encoded), "RTP rejects invalid payload type");
}
AmrFrame frame(AmrCodec codec, uint8_t ft) {
    AmrFrame f; f.frameType = ft; const int bits = amrSpeechBits(codec, ft);
    if (bits >= 0) {
        f.speech.resize((bits + 7)/8);
        for (std::size_t i = 0; i < f.speech.size(); ++i) f.speech[i] = uint8_t(0xa5 + i * 17);
        if (bits % 8) f.speech.back() &= uint8_t(0xff << (8 - bits % 8));
    }
    return f;
}
void amrTests() {
    bool all = true;
    for (auto codec : {AmrCodec::Narrowband, AmrCodec::Wideband}) {
        for (uint8_t ft = 0; ft < 16; ++ft) {
            if (amrSpeechBits(codec, ft) < 0) continue;
            AmrPayload p; p.frames = {frame(codec, ft), frame(codec, 15)};
            for (auto alignment : {AmrAlignment::OctetAligned, AmrAlignment::BandwidthEfficient}) {
                Bytes wire, storage; AmrPayload parsed; AmrFrame stored;
                all &= serializeAmr(p, codec, alignment, wire) && parseAmr(wire,codec,alignment,parsed) && parsed.frames.size() == 2;
                all &= parsed.frames[0].speech == p.frames[0].speech && parsed.frames[0].frameType == ft && parsed.frames[1].speech.empty();
                all &= amrToStorage(p.frames[0],codec,storage) && amrFromStorage(storage,codec,stored) && stored.speech == p.frames[0].speech;
                wire.push_back(0); all &= !parseAmr(wire,codec,alignment,parsed);
            }
        }
    }
    check(all, "AMR NB/WB all speech/SID/no-data/lost frame types in both alignments and storage");
    AmrPayload p; p.frames = {{0,true,Bytes(12,0)}}; Bytes b;
    const Bytes oa{0xf0,0x04,0,0,0,0,0,0,0,0,0,0,0,0};
    const Bytes be{0xf0,0x40,0,0,0,0,0,0,0,0,0,0,0,0};
    check(serializeAmr(p,AmrCodec::Narrowband,AmrAlignment::OctetAligned,b) && b == oa, "AMR OA FT0 known zero-speech vector");
    check(serializeAmr(p,AmrCodec::Narrowband,AmrAlignment::BandwidthEfficient,b) && b == be, "AMR BE FT0 known bit-packed vector");
    AmrPayload parsed;
    auto reserved = oa; reserved[0] |= 15; reserved[1] |= 3; reserved.back() |= 1;
    check(parseAmr(reserved,AmrCodec::Narrowband,AmrAlignment::OctetAligned,parsed) && parsed.frames[0].speech.back()==0,
          "AMR receiver ignores reserved/padding bits as RFC4867 requires");
    check(!parseAmr({0xf0,0x4c},AmrCodec::Narrowband,AmrAlignment::OctetAligned,parsed), "AMR rejects reserved NB FT9");
    check(!parseAmr({0xf0,0x84},AmrCodec::Narrowband,AmrAlignment::OctetAligned,parsed), "AMR rejects unterminated ToC");
    check(!parseAmr({0xf0,0x04},AmrCodec::Narrowband,AmrAlignment::OctetAligned,parsed), "AMR rejects truncated speech");
    p.cmr=8; check(!serializeAmr(p,AmrCodec::Narrowband,AmrAlignment::OctetAligned,b), "AMR sender rejects reserved CMR");
}
void redTests() {
    RedPayload p; p.redundant = {{7,160,Bytes(14,0x11)}}; p.primary = {5,0,Bytes(84,0x22)};
    Bytes b; RedPayload decoded;
    check(serializeRed(p,b) && b.size()==103 && b[0]==0x87 && b[1]==2 && b[2]==0x80 && b[3]==14 && b[4]==5,
          "RED RFC2198 section 7 header vector (LPC 14 bytes + DVI 84)");
    check(parseRed(b,decoded) && decoded.redundant[0].timestampOffset==160 && decoded.redundant[0].payload==p.redundant[0].payload &&
          decoded.primary.payload==p.primary.payload, "RED redundant and primary blocks round-trip");
    check(!parseRed({0x87,0,0,1},decoded) && !parseRed({0x87,0,0,8,5,0},decoded), "RED rejects missing primary/truncated block");
    p.redundant[0].timestampOffset=16384; check(!serializeRed(p,b), "RED rejects timestamp offset overflow");
    p.redundant[0].timestampOffset=0; p.redundant[0].payload.resize(1024); check(!serializeRed(p,b), "RED rejects 10-bit length overflow");
}
void rtcpTests() {
    RtcpPacket rr; rr.ssrc=0x10203040;
    rr.reports.push_back({0xaabbccdd,64,-1,65537,20,0x12345678,65536});
    Bytes b; std::vector<RtcpPacket> parsed;
    const Bytes expected{0x81,201,0,7,0x10,0x20,0x30,0x40,0xaa,0xbb,0xcc,0xdd,64,0xff,0xff,0xff,
        0,1,0,1,0,0,0,20,0x12,0x34,0x56,0x78,0,1,0,0};
    check(serializeRtcp(rr,b) && b==expected, "RTCP RR known network-order vector with signed 24-bit loss");
    check(parseRtcp(b,parsed) && parsed.size()==1 && parsed[0].reports[0].cumulativeLost==-1, "RTCP negative loss round-trip");
    RtcpPacket sr; sr.type=200; sr.ssrc=7; sr.sender={0x0102030405060708ull,123,456,789}; sr.reports=rr.reports;
    check(serializeRtcp(sr,b) && parseRtcp(b,parsed) && parsed[0].sender.ntpTimestamp==sr.sender.ntpTimestamp && parsed[0].sender.octetCount==789,
          "RTCP SR sender fields and reports round-trip");
    RtcpPacket xr; xr.type=207; xr.ssrc=9;
    xr.xrBlocks={makeXrReceiverReferenceTime(0x0102030405060708ull),makeXrDlrr({{1,2,3},{4,5,6}})};
    Bytes xb; check(serializeRtcp(xr,xb) && parseRtcp(xb,parsed) && parsed[0].xrBlocks.size()==2 && parsed[0].xrBlocks[1].data.size()==24,
          "RTCP XR RRTR/DLRR block round-trip");
    b.insert(b.end(),xb.begin(),xb.end()); check(parseRtcp(b,parsed) && parsed.size()==2, "RTCP parses multiple report packets");
    auto bad=expected; bad[3]=8; check(!parseRtcp(bad,parsed), "RTCP rejects packet length overrun");
    bad=expected; bad[0]=0x82; check(!parseRtcp(bad,parsed), "RTCP rejects report count mismatch");
    bad=xb; bad[11]=0xff; check(!parseRtcp(bad,parsed), "RTCP rejects XR block overrun");
    ReceiverStatistics stats(42);
    check(stats.observe(65534,0xfffffff0u,0xfffffff0u) && stats.observe(0,0x130,0x130) && stats.observe(65535,0x90,0x90),
          "RTCP sequence tracker accepts wrap and prior-cycle reordering");
    auto report=stats.report(0); check(report.extendedHighestSequence==65536 && report.cumulativeLost==0 && report.jitter==0,
          "RTCP wrapped extended sequence and no false loss");
    stats.observe(2,0x270,0x280); report=stats.report(0);
    check(report.cumulativeLost==1 && report.fractionLost==128 && report.jitter==1, "RTCP interval loss fraction and RFC jitter update");
    stats.observe(2,0x270,0x280); stats.observe(2,0x270,0x280); report=stats.report(0);
    check(report.cumulativeLost==-1 && report.fractionLost==0, "RTCP includes duplicates without negative fraction loss");
    stats.noteSenderReport(0x0102030405060708ull,1000); report=stats.report(2500);
    check(report.lastSr==0x03040506 && report.delaySinceLastSr==98304, "RTCP LSR middle NTP bits and 1/65536-second DLSR");
    check(!stats.observe(20000,0,0) && stats.observe(20001,0,0) && stats.report(0).cumulativeLost==0, "RTCP requires two packets for source restart");
    stats.reset(42); RtpPacket wrong; wrong.ssrc=3;
    check(!stats.observe(wrong,0,48000) && stats.receivedPackets()==0, "RTCP ignores wrong SSRC");
}
void networkTests() {
    NetworkProfile p; p.lossRate=0.2; p.jitterAmplitudeMs=12; p.baseDelayMs=30; p.jitterAutocorrelation=0.7;
    NetworkSimulator a(p,123),b(p,123); bool repeat=true;
    for(int i=0;i<1000;++i) repeat &= a.nextLoss()==b.nextLoss() && a.nextJitterMs()==b.nextJitterMs();
    check(repeat, "network seeded loss/jitter realization is deterministic");
    NetworkProfile independent=p; independent.jitterAmplitudeMs=0; independent.duplicateProbability=1;
    NetworkSimulator c(independent,123);a.reset(123);bool independentLoss=true;
    for(int i=0;i<1000;++i){independentLoss &= a.nextLoss()==c.nextLoss();a.nextJitterMs();c.nextJitterMs();}
    check(independentLoss,"jitter/duplication knobs do not alter seeded packet-loss realization");
    a.reset(123); b.reset(123); check(a.nextJitterMs()==b.nextJitterMs(), "network reset reproduces realization");
    p.lossModel=NetworkLossModel::GilbertElliott; p.gilbert=GilbertElliottParameters::fromLossAndBurst(0.2,5);
    a.configure(p); a.reset(567); int lost=0,bursts=0; bool before=false;
    for(int i=0;i<200000;++i) { bool l=a.nextLoss(); lost+=l; bursts+=l&&!before; before=l; }
    check(std::abs(double(lost)/200000-0.2)<0.008 && std::abs(double(lost)/bursts-5)<0.15,
          "Gilbert-Elliott measured stationary loss and mean burst match parameters");
    p.gilbert={0,0,0.25,0}; a.configure(p); a.reset(678); lost=0;
    for(int i=0;i<100000;++i) lost+=a.nextLoss();
    check(std::abs(double(lost)/100000-0.75)<0.01, "Gilbert-Elliott k/h are emission success probabilities");
    p.lossModel=NetworkLossModel::ErrorPattern; a.configure(p);
    check(a.setErrorPattern("0 1 1\n0") && !a.nextLoss() && a.nextLoss() && a.nextLoss() && !a.nextLoss() && !a.nextLoss(),
          "network replays supplied loss trace exactly");
    check(!a.setErrorPattern("EP1") && !a.setErrorPattern(""), "network rejects invalid/empty error pattern rather than inventing preset");
    bool distributions=true;
    for(auto d:{JitterDistribution::Uniform,JitterDistribution::Gamma,JitterDistribution::Weibull,JitterDistribution::Pareto}) {
        p.jitterDistribution=d;p.jitterShape=4;p.jitterAutocorrelation=0;p.jitterAmplitudeMs=10;
        a.configure(p); a.reset(456); double sum=0; bool finite=true;
        for(int i=0;i<100000;++i) { const double v=a.nextJitterMs();sum+=v;finite&=std::isfinite(v); }
        distributions &= finite && std::abs(sum/100000)<0.15;
    }
    check(distributions,"all jitter distributions produce finite approximately zero-centred offsets");
    p.jitterDistribution=JitterDistribution::Uniform;p.jitterAutocorrelation=0.8;a.configure(p);a.reset(789);
    double sum=0,squares=0,lag=0,prev=a.nextJitterMs();
    for(int i=0;i<100000;++i){double v=a.nextJitterMs();sum+=v;squares+=v*v;lag+=v*prev;prev=v;}
    const double mean=sum/100000; const double corr=(lag/100000-mean*mean)/(squares/100000-mean*mean);
    check(std::abs(corr-0.8)<0.02,"jitter AR(1) measured lag-one autocorrelation matches rho");
    p={};p.jitterAmplitudeMs=100;p.baseDelayMs=100;p.duplicateProbability=1;p.duplicateDelayMs=1;p.maxQueuedPackets=1024;
    a.configure(p);a.reset(123);
    for(uint64_t i=0;i<200;++i) a.submit({i,uint32_t(i*160),double(i*20),0,false,{uint8_t(i)}});
    NetworkPacket packet; int delivered=0; double last=-1; bool monotonic=true;
    while(a.popArrived(10000,packet)){++delivered;monotonic&=packet.arrivalTimeMs>=last&&packet.arrivalTimeMs>=packet.sentTimeMs;last=packet.arrivalTimeMs;}
    check(delivered==400 && monotonic && a.counters().reordered>0 && a.counters().duplicated==200,
          "arrival queue reorders/duplicates without time travel or unstable arrival ordering");
    p={};p.lateProbability=1;p.lateExtraDelayMs=200;a.configure(p);a.reset();
    a.submit({0,0,0,0,false,{1}}); check(!a.popArrived(199,packet)&&a.popArrived(200,packet),"explicit late-packet injection delays arrivals");
    PacketJitterBuffer queue(3);queue.reset(65535);
    NetworkPacket first{65535,0,0,20,false,{1}}, second{65536,160,20,25,false,{2}};
    check(queue.push(second)&&queue.push(first)&&!queue.push(first)&&queue.peek(65536,30,packet)&&packet.bytes==Bytes{2},
          "sequence-sorted jitter buffer retains reordered data, rejects duplicates, supports FEC peek");
    check(queue.take(65535,30,packet)&&packet.bytes==Bytes{1}&&queue.take(65536,40,packet)&&packet.bytes==Bytes{2},
          "jitter buffer playout follows extended sequence through RTP wrap");
    check(!queue.push(first)&&!queue.take(65537,60,packet)&&queue.counters().missing==1,"jitter buffer classifies late and missing data");
    queue.push({65538,0,0,200,false,{1}});check(!queue.take(65538,100,packet)&&queue.counters().late==2,"jitter buffer discards frames after deadline");
    p={};p.maxQueuedPackets=1;a.configure(p);a.reset();a.submit({0,0,0,0,false,{}});
    check(!a.submit({1,0,0,0,false,{}})&&a.counters().overflow==1&&a.queued()==1,"network queue is capacity bounded");
    p.jitterShape=std::numeric_limits<double>::quiet_NaN();p.lossRate=std::numeric_limits<double>::infinity();p.jitterAmplitudeMs=-100;
    const auto safe=p.sanitized();check(std::isfinite(safe.jitterShape)&&safe.lossRate==0&&safe.jitterAmplitudeMs==0,"nonfinite network controls sanitized");
}
void malformedFuzz() {
    std::mt19937 random(991); bool stable=true;
    for(int i=0;i<20000;++i) {
        Bytes b(random()%256);for(auto& v:b)v=uint8_t(random());
        RtpPacket rtp; AmrPayload amr; RedPayload red;std::vector<RtcpPacket> rtcp;
        parseRtp(b,rtp);parseAmr(b,AmrCodec::Narrowband,AmrAlignment::BandwidthEfficient,amr);
        parseAmr(b,AmrCodec::Wideband,AmrAlignment::OctetAligned,amr);parseRed(b,red);parseRtcp(b,rtcp);
        if(rtp.payload.size()>b.size())stable=false;
    }
    check(stable,"20,000 deterministic malformed packet inputs complete safely");
}
}
int main(){rtpTests();amrTests();redTests();rtcpTests();networkTests();malformedFuzz();return failures?1:0;}
