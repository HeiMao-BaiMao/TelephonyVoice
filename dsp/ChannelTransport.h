#pragma once
#include "dsp/ICodec.h"
#include "dsp/NetworkProfile.h"
#include "dsp/AdvancedControls.h"
#include "dsp/Transport.h"
#include "dsp/TransportEvs.h"
#include <map>

namespace TelephonyDSP {
// Owns encoded packet scheduling. Synchronous exchange includes fixed playout
// prebuffering; batch ingress hands actual arrivals directly to a codec JBM.
class ChannelTransport final : public ICodecTransport {
public:
    void configure(const AdvancedSettings&, double loss);
    void reset();
    bool exchange(const CodecPacketInfo&,const uint8_t*,size_t,bool,CodecPlayout&) override;
    bool supportsBatchIngress() const override { return true; }
    void exchangeBatch(const std::vector<CodecEncodedPacket>&, double receiverDeadlineMs,
                       std::vector<CodecPlayout>& arrivals) override;
    bool warmingUp() const { return warming; }
    bool lastPacketReceived() const { return lastReceived; }
    ProcessingTelemetry telemetry() const;
    void setTime(double seconds, bool playing);
    const Transport::Bytes& receiverReportBytes() const { return lastReceiverReport; }
    const Transport::Bytes& senderReportBytes() const { return lastSenderReport; }
    const Transport::Bytes& extendedReportBytes() const { return lastExtendedReport; }
    const Transport::ReceptionReport& receiverReport() const { return lastReceptionReport; }
    const NetworkCounters& networkCounters() const { return network.counters(); }
    std::size_t pendingPackets() const { return network.queued(); }
private:
    AdvancedSettings settings;
    NetworkSimulator network;
    PacketJitterBuffer jitter;
    uint64_t sequence=0, playoutSequence=0;
    double clockMs=0, lastTransit=0, jitterEstimate=0, sessionStartMs=0, nextReportMs=0;
    double hostTimeMs=0; bool haveHostTime=false, hostPlaying=false;
    bool warming=false, haveTransit=false, lastReceived=false, batchMode=false, batchStarted=false;
    uint64_t received=0, missed=0;
    uint64_t batchSubmitted=0, batchDuplicates=0;
    uint32_t lastSenderTimestamp=0;
    uint64_t sentPackets=0, sentOctets=0;
    struct Metadata {
        bool dtx=false; CodecPacketInfo info{}; uint32_t timestamp=0;
        bool packetized=false, octetAligned=true, headerFullOnly=false;
        double sentTimeMs=0;
        uint64_t sourceSequence=0;
        double sourceTimestampMs=0;
        uint8_t pending=0; // original plus any queued duplicate
        bool delivered=false;
    };
    std::map<uint64_t,Metadata> metadata;
    std::vector<uint8_t> previousPayload;
    uint32_t previousTimestamp=0;
    uint8_t previousPayloadType=0;
    Transport::ReceiverStatistics receiverStats{0x54454c45u};
    std::vector<uint8_t> lastReceiverReport, lastSenderReport, lastExtendedReport;
    Transport::ReceptionReport lastReceptionReport;
    bool encodeWire(const CodecPacketInfo&,const uint8_t*,size_t,uint16_t,uint32_t,std::vector<uint8_t>&);
    bool decodeWire(const Metadata&,const std::vector<uint8_t>&,std::vector<uint8_t>&);
    void observeArrival(const NetworkPacket&, const Metadata&);
    void updateReports(double nowMs, uint32_t timestamp);
};
}
