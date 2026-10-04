#include "dsp/ChannelTransport.h"
#include <cmath>
#include <algorithm>

namespace TelephonyDSP {
using A=AdvancedControl;
void ChannelTransport::configure(const AdvancedSettings& s,double loss) {
    settings=s;
    NetworkProfile p;
    p.lossRate=loss;
    p.lossRateBoost=s.get(A::LossBoost);
    p.burstLengthMean=s.get(A::BurstMean);
    p.jitterAmplitudeMs=s.get(A::JitterMs);
    // Negative jitter is early relative to the nominal path, never before send.
    p.baseDelayMs=p.jitterAmplitudeMs;
    p.jitterDistribution=static_cast<JitterDistribution>((int)s.get(A::JitterDistribution));
    p.jitterShape=s.get(A::JitterShape);
    p.jitterAutocorrelation=s.get(A::JitterCorrelation);
    p.duplicateProbability=s.get(A::DuplicateRate);
    p.opusPlaybackDelayMs=s.get(A::PlaybackDelayMs);
    p.maxDelayMs=500;
    p.lateProbability=s.get(A::LateProbability);
    p.lateExtraDelayMs=s.get(A::LateDelayMs);
    if(s.enabled(A::GilbertEnabled)) {
        p.lossModel=NetworkLossModel::GilbertElliott;
        p.gilbert={s.get(A::GilbertP),s.get(A::GilbertR),1-s.get(A::GilbertGoodLoss),1-s.get(A::GilbertBadLoss)};
    } else if(p.burstLengthMean>1) {
        p.lossModel=NetworkLossModel::GilbertElliott;
        p.gilbert=GilbertElliottParameters::fromLossAndBurst(p.lossRate,p.burstLengthMean);
    }
    network.configure(p);
}
void ChannelTransport::reset() {
    network.reset(); jitter.reset(); metadata.clear(); previousPayload.clear();
    sequence=playoutSequence=received=missed=sentPackets=sentOctets=0;
    clockMs=lastTransit=jitterEstimate=sessionStartMs=nextReportMs=0;
    warming=false; haveTransit=false; haveHostTime=false; lastReceived=false;
    batchMode=batchStarted=false; batchSubmitted=batchDuplicates=0; lastSenderTimestamp=0;
    previousTimestamp=0; previousPayloadType=0; lastReceptionReport={};
    lastReceiverReport.clear(); lastSenderReport.clear(); lastExtendedReport.clear();
    receiverStats.reset(0x54454c45u);
}
void ChannelTransport::setTime(double seconds,bool playing) {
    if(!std::isfinite(seconds)) return;
    const double next=seconds*1000;
    if(haveHostTime && (next<hostTimeMs-1 || next>hostTimeMs+2000)) reset();
    hostTimeMs=next; hostPlaying=playing; haveHostTime=true;
}
namespace {
uint8_t payloadType(CodecPacketFormat f) {
    switch(f) { case CodecPacketFormat::G711MuLaw:return 0; case CodecPacketFormat::G711ALaw:return 8;
    case CodecPacketFormat::GSM:return 3; case CodecPacketFormat::AMRNB:return 96; case CodecPacketFormat::AMRWB:return 97;
    case CodecPacketFormat::EVSG192:return 98; case CodecPacketFormat::Opus:return 111; case CodecPacketFormat::LinearPcm16:return 113; } return 96;
}
uint32_t rtpClockRate(const CodecPacketInfo& info) {
    // TS26.445 A.3.2 and RFC7587 4.1; independent of PCM sample rate.
    if(info.format==CodecPacketFormat::EVSG192) return 16000;
    if(info.format==CodecPacketFormat::Opus) return 48000;
    return uint32_t(info.sampleRate);
}
uint32_t timestampAt(double milliseconds,uint32_t rate) {
    double ticks=std::fmod(std::round(milliseconds*rate/1000),4294967296.0);
    if(ticks<0) ticks+=4294967296.0;
    return uint32_t(ticks);
}
}
bool ChannelTransport::encodeWire(const CodecPacketInfo& info,const uint8_t* data,size_t count,uint16_t wireSequence,uint32_t timestamp,std::vector<uint8_t>& out) {
    using namespace Transport;
    if(count && !data) return false;
    Bytes payload; if(count) payload.assign(data,data+count);
    if(!settings.enabled(A::PacketFormat)) { out=std::move(payload); return true; }
    if(info.format==CodecPacketFormat::AMRNB || info.format==CodecPacketFormat::AMRWB) {
        const auto kind=info.format==CodecPacketFormat::AMRNB?AmrCodec::Narrowband:AmrCodec::Wideband;
        AmrFrame frame;
        if(!amrFromStorage(payload,kind,frame)) return false;
        AmrPayload amr; amr.frames.push_back(frame);
        if(!serializeAmr(amr,kind,settings.enabled(A::AmrOctetAligned)?AmrAlignment::OctetAligned:AmrAlignment::BandwidthEfficient,payload)) return false;
    } else if(info.format==CodecPacketFormat::EVSG192) {
        EvsFrame frame; EvsG192Options g192;
        g192.mode=info.amrWbIo?EvsMode::AmrWbIo:EvsMode::Primary;
        if(info.amrWbIo && info.amrWbSidMode>=0) g192.sid=EvsAmrWbSid{info.amrWbSidUpdate,(uint8_t)info.amrWbSidMode};
        if(!evsFrameFromG192(payload,frame,g192)) return false;
        EvsPayloadOptions options;
        options.format=static_cast<EvsPayloadFormat>((int)settings.get(A::EvsPayloadStyle));
        options.headerFullOnly=options.format==EvsPayloadFormat::HeaderFull;
        // SID/NO_DATA can require Header-Full even in a compact-preferred
        // session. Auto emits compact whenever the current frame permits it.
        if(options.format==EvsPayloadFormat::Compact) options.format=EvsPayloadFormat::Auto;
        if(!serializeEvsPayload({frame},payload,options)) return false;
    }
    RtpPacket packet; packet.sequence=wireSequence; packet.timestamp=timestamp;
    packet.ssrc=0x54454c45u; packet.payloadType=payloadType(info.format);
    if(settings.enabled(A::Redundancy)) {
        RedPayload red; red.primary={packet.payloadType,0,payload};
        const uint32_t offset=timestamp-previousTimestamp;
        if(!previousPayload.empty() && previousPayloadType==packet.payloadType && offset<=16383 && previousPayload.size()<=1023)
            red.redundant.push_back({previousPayloadType,(uint16_t)offset,previousPayload});
        previousPayload=payload; previousTimestamp=timestamp; previousPayloadType=packet.payloadType;
        if(!serializeRed(red,packet.payload)) return false;
        packet.payloadType=112;
    } else { packet.payload=std::move(payload); previousPayload.clear(); }
    if(!serializeRtp(packet,out)) return false;
    ++sentPackets; sentOctets+=packet.payload.size();
    return true;
}
bool ChannelTransport::decodeWire(const Metadata& metadata,const std::vector<uint8_t>& in,std::vector<uint8_t>& out) {
    using namespace Transport;
    const auto& info=metadata.info;
    if(!metadata.packetized) { out=in; return true; }
    RtpPacket packet; if(!parseRtp(in,packet)) return false;
    if(packet.ssrc!=0x54454c45u) return false;
    Bytes payload=std::move(packet.payload);
    if(packet.payloadType==112) { RedPayload red; if(!parseRed(payload,red)) return false; packet.payloadType=red.primary.payloadType; payload=std::move(red.primary.payload); }
    if(packet.payloadType!=payloadType(info.format)) return false;
    if(info.format==CodecPacketFormat::AMRNB || info.format==CodecPacketFormat::AMRWB) {
        const auto kind=info.format==CodecPacketFormat::AMRNB?AmrCodec::Narrowband:AmrCodec::Wideband;
        AmrPayload amr;
        if(!parseAmr(payload,kind,metadata.octetAligned?AmrAlignment::OctetAligned:AmrAlignment::BandwidthEfficient,amr) || amr.frames.size()!=1) return false;
        return amrToStorage(amr.frames[0],kind,out);
    }
    if(info.format==CodecPacketFormat::EVSG192) {
        EvsPayload evs; EvsPayloadOptions options;
        options.headerFullOnly=metadata.headerFullOnly;
        if(!parseEvsPayload(payload,evs,options) || evs.frames.size()!=1) return false;
        EvsG192Frame frame; if(!evsFrameToG192(evs.frames[0],frame)) return false;
        out=std::move(frame.data); return true;
    }
    out=std::move(payload); return true;
}
void ChannelTransport::observeArrival(const NetworkPacket& arrived,const Metadata& meta) {
    const double transit=arrived.arrivalTimeMs-arrived.sentTimeMs;
    if(haveTransit) jitterEstimate+=(std::abs(transit-lastTransit)-jitterEstimate)/16;
    lastTransit=transit; haveTransit=true;
    if(meta.packetized) {
        Transport::RtpPacket rtp;
        if(Transport::parseRtp(arrived.bytes,rtp))
            receiverStats.observe(rtp,arrived.arrivalTimeMs,rtpClockRate(meta.info));
    }
}
void ChannelTransport::updateReports(double now,uint32_t timestamp) {
    if(now<nextReportMs) return;
    nextReportMs=now+1000;
    lastReceptionReport=receiverStats.report(now);
    Transport::RtcpPacket rr; rr.ssrc=0x52584356u; rr.reports.push_back(lastReceptionReport);
    Transport::serializeRtcp(rr,lastReceiverReport);
    Transport::RtcpPacket sr; sr.type=200; sr.ssrc=0x54454c45u;
    const uint64_t ntp=(uint64_t)((now-sessionStartMs)/1000.0*4294967296.0);
    sr.sender={ntp,timestamp,(uint32_t)sentPackets,(uint32_t)sentOctets};
    Transport::serializeRtcp(sr,lastSenderReport);
    receiverStats.noteSenderReport(ntp,now); // in-process zero-delay control path
    Transport::RtcpPacket xr; xr.type=207; xr.ssrc=0x52584356u;
    xr.xrBlocks.push_back(Transport::makeXrReceiverReferenceTime(ntp));
    Transport::serializeRtcp(xr,lastExtendedReport);
}
void ChannelTransport::exchangeBatch(const std::vector<CodecEncodedPacket>& packets,
                                    double receiverDeadlineMs,std::vector<CodecPlayout>& arrivals) {
    arrivals.clear(); lastReceived=false; warming=false;
    if(!std::isfinite(receiverDeadlineMs) || receiverDeadlineMs<0) return;
    batchMode=true;
    if(!batchStarted) {
        sessionStartMs=settings.enabled(A::HostClock)&&haveHostTime?hostTimeMs:0;
        nextReportMs=sessionStartMs;
        batchStarted=true;
    } else if(receiverDeadlineMs<clockMs-sessionStartMs) return;
    const double now=sessionStartMs+receiverDeadlineMs;
    clockMs=now;
    for(const auto& packet:packets) {
        ++batchSubmitted;
        const auto& info=packet.info;
        if(info.sampleRate<=0 || info.frameSamples<=0 || info.sampleRate>384000 ||
           info.frameSamples>info.sampleRate || !std::isfinite(packet.sourceTimestampMs)) {
            ++missed; continue;
        }
        const uint64_t current=sequence++;
        const uint32_t timestamp=timestampAt(sessionStartMs+packet.sourceTimestampMs,rtpClockRate(info));
        Metadata meta{info.dtx,info,timestamp,settings.enabled(A::PacketFormat),
                      settings.enabled(A::AmrOctetAligned),settings.get(A::EvsPayloadStyle)==2,now};
        meta.sourceSequence=packet.sequence; meta.sourceTimestampMs=packet.sourceTimestampMs;
        NetworkPacket submitted;
        submitted.sequence=current; submitted.timestamp=timestamp;
        // This is the independent receiver/physical clock. Using the source
        // RTP timestamp here would cancel sender drift at the arrival queue.
        submitted.sentTimeMs=now;
        const bool encoded=encodeWire(info,packet.payload.data(),packet.payload.size(),
                                      uint16_t(packet.sequence),timestamp,submitted.bytes);
        if(encoded) {
            if(meta.packetized) lastSenderTimestamp=timestamp;
            const auto duplicatesBefore=network.counters().duplicated;
            if(network.submit(std::move(submitted),packet.lost))
                meta.pending=uint8_t(1+network.counters().duplicated-duplicatesBefore);
        }
        if(!meta.pending) ++missed;
        metadata[current]=meta;
    }
    NetworkPacket arrived;
    while(network.popArrived(now,arrived)) {
        auto found=metadata.find(arrived.sequence);
        if(found==metadata.end()) continue;
        auto& meta=found->second;
        if(meta.pending) --meta.pending;
        if(arrived.duplicate) ++batchDuplicates;
        else ++received;
        observeArrival(arrived,meta);
        const auto makePlayout=[&](const Metadata& m) {
            CodecPlayout result;
            result.dtx=m.dtx; result.hasTiming=true; result.sequence=m.sourceSequence;
            result.sourceTimestampMs=m.sourceTimestampMs;
            result.sentTimeMs=m.sentTimeMs-sessionStartMs;
            result.arrivalTimeMs=arrived.arrivalTimeMs-sessionStartMs;
            result.playoutTimeMs=receiverDeadlineMs;
            return result;
        };
        // Recover a missing previous source packet from an actual arriving
        // RED block. Feed it at this block's arrival, leaving the real JBM to
        // accept or reject it. Recovery does not fabricate an RTP reception.
        if(meta.packetized && arrived.sequence>0) {
            auto previous=metadata.find(arrived.sequence-1);
            if(previous!=metadata.end() && previous->second.packetized && !previous->second.delivered) {
                Transport::RtpPacket rtp; Transport::RedPayload red;
                if(Transport::parseRtp(arrived.bytes,rtp) && rtp.payloadType==112 &&
                   Transport::parseRed(rtp.payload,red)) {
                    auto& old=previous->second;
                    for(const auto& block:red.redundant) {
                        if(rtp.timestamp-block.timestampOffset!=old.timestamp) continue;
                        rtp.payloadType=block.payloadType; rtp.payload=block.payload;
                        rtp.timestamp=old.timestamp; rtp.sequence=uint16_t(old.sourceSequence);
                        std::vector<uint8_t> wire;
                        auto recovered=makePlayout(old);
                        if(Transport::serializeRtp(rtp,wire) && decodeWire(old,wire,recovered.payload)) {
                            old.delivered=true; arrivals.push_back(std::move(recovered));
                        }
                        break;
                    }
                }
            }
        }
        auto result=makePlayout(meta);
        if(decodeWire(meta,arrived.bytes,result.payload)) {
            meta.delivered=true;
            arrivals.push_back(std::move(result));
        } else if(!arrived.duplicate) ++missed;
    }
    if(sentPackets) updateReports(now,lastSenderTimestamp);
    // Retain queued originals even if an unusual caller submits more than
    // the normal 0/1/2 frames. Settled history is bounded independently.
    const uint64_t oldest=sequence>1024?sequence-1024:0;
    for(auto it=metadata.begin();it!=metadata.end() && it->first<oldest;)
        if(!it->second.pending) it=metadata.erase(it); else ++it;
    lastReceived=!arrivals.empty();
}
bool ChannelTransport::exchange(const CodecPacketInfo& info,const uint8_t* data,size_t bytes,bool lost,CodecPlayout& result) {
    result={}; lastReceived=false;
    if(info.sampleRate<=0 || info.frameSamples<=0 || info.sampleRate>384000 || info.frameSamples>info.sampleRate || (bytes&&!data)) {
        warming=false; return false;
    }
    const double frameMs=1000.0*info.frameSamples/info.sampleRate;
    const uint64_t delayFrames=(uint64_t)std::ceil(settings.get(A::PlaybackDelayMs)/frameMs);
    const uint64_t current=sequence++;
    if(current==0) {
        if(settings.enabled(A::HostClock) && haveHostTime) clockMs=hostTimeMs;
        sessionStartMs=clockMs; nextReportMs=clockMs;
    }
    const double now=clockMs;
    const uint32_t timestamp=timestampAt(now,rtpClockRate(info));
    const Metadata currentMeta{info.dtx,info,timestamp,settings.enabled(A::PacketFormat),settings.enabled(A::AmrOctetAligned),settings.get(A::EvsPayloadStyle)==2,now};
    metadata[current]=currentMeta;
    NetworkPacket submitted; submitted.sequence=current; submitted.timestamp=timestamp; submitted.sentTimeMs=now;
    // Packetize even a lost packet: its encoded data is available to RED in
    // the following packet, and sender counters include network losses.
    if(encodeWire(info,data,bytes,uint16_t(current),timestamp,submitted.bytes)) network.submit(std::move(submitted),lost);
    NetworkPacket arrived;
    while(network.popArrived(now,arrived)) {
        const auto arrivalMeta=metadata.find(arrived.sequence);
        if(arrivalMeta!=metadata.end()) observeArrival(arrived,arrivalMeta->second);
        jitter.push(std::move(arrived));
    }
    clockMs+=frameMs;
    warming=current<delayFrames;
    if(warming) return false;
    const uint64_t wanted=playoutSequence++;
    auto meta=metadata.find(wanted);
    const Metadata packetMeta=meta==metadata.end()?currentMeta:meta->second;
    result.dtx=meta!=metadata.end() && meta->second.dtx;
    bool ok=jitter.take(wanted,now,arrived) && decodeWire(packetMeta,arrived.bytes,result.payload);
    result.hasTiming=true; result.sequence=wanted;
    result.sentTimeMs=packetMeta.sentTimeMs-sessionStartMs;
    result.sourceTimestampMs=result.sentTimeMs;
    result.arrivalTimeMs=(ok?arrived.arrivalTimeMs:now)-sessionStartMs;
    result.playoutTimeMs=now-sessionStartMs;
    NetworkPacket future;
    if(jitter.peek(wanted+1,now,future)) {
        auto futureMeta=metadata.find(wanted+1);
        const Metadata nextMeta=futureMeta==metadata.end()?currentMeta:futureMeta->second;
        decodeWire(nextMeta,future.bytes,result.nextPayload);
        // Recover the previous payload from the next packet's RFC2198 block.
        if(!ok && packetMeta.packetized && nextMeta.packetized) {
            Transport::RtpPacket rtp; Transport::RedPayload red;
            if(Transport::parseRtp(future.bytes,rtp) && rtp.payloadType==112 && Transport::parseRed(rtp.payload,red)) {
                for(const auto& block:red.redundant) {
                    if(rtp.timestamp-block.timestampOffset!=packetMeta.timestamp) continue;
                    rtp.payloadType=block.payloadType; rtp.payload=block.payload; rtp.timestamp=packetMeta.timestamp;
                    rtp.sequence=uint16_t(wanted);
                    std::vector<uint8_t> recovered;
                    if(Transport::serializeRtp(rtp,recovered)) {
                        ok=decodeWire(packetMeta,recovered,result.payload);
                        if(ok) result.arrivalTimeMs=future.arrivalTimeMs-sessionStartMs;
                    }
                    break;
                }
            }
        }
    }
    if(ok) ++received; else ++missed;
    if(currentMeta.packetized) updateReports(now,timestamp);
    // Retain a bounded history for late arrivals' original payload/clock mode.
    if(wanted>=1024) metadata.erase(metadata.begin(),metadata.upper_bound(wanted-1024));
    lastReceived=ok; return ok;
}
ProcessingTelemetry ChannelTransport::telemetry() const {
    ProcessingTelemetry t;
    // Batch ingress has no transport playout deadline: the real codec JBM
    // owns that decision. Count actual submissions/losses, never an empty
    // receiver tick or a still-in-flight packet as a missing media frame.
    t.packets=batchMode?batchSubmitted:received+missed; t.lost=missed;
    t.measuredLoss=t.packets?double(t.lost)/t.packets:0;
    t.duplicates=batchMode?batchDuplicates:jitter.counters().duplicates;
    t.late=batchMode?0:jitter.counters().late;
    t.jitterMs=jitterEstimate; return t;
}
}
