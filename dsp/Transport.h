#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// In-process wire formats only; no UDP, SDP or socket I/O. Specifications:
// https://www.rfc-editor.org/rfc/rfc3550 (RTP and RTCP)
// https://www.rfc-editor.org/rfc/rfc4867 (single-channel AMR/AMR-WB)
// https://www.rfc-editor.org/rfc/rfc2198 (RED)
// https://www.rfc-editor.org/rfc/rfc3611 (XR)
namespace TelephonyDSP { namespace Transport {
using Bytes = std::vector<uint8_t>;
struct RtpPacket {
    bool marker = false;
    uint8_t payloadType = 96;
    uint16_t sequence = 0;
    uint32_t timestamp = 0, ssrc = 0;
    std::vector<uint32_t> csrcs;
    bool hasExtension = false;
    uint16_t extensionProfile = 0;
    Bytes extension; // multiple of four octets
    uint8_t padding = 0; // total padding octets, including final count
    Bytes payload;
};
bool serializeRtp(const RtpPacket&, Bytes&, std::string* error = nullptr);
bool parseRtp(const Bytes&, RtpPacket&, std::string* error = nullptr);
// Half-range comparison, valid when packets are within 32767 positions.
bool sequenceBefore(uint16_t a, uint16_t b);

enum class AmrCodec { Narrowband, Wideband };
enum class AmrAlignment { OctetAligned, BandwidthEfficient };
struct AmrFrame {
    uint8_t frameType = 0;
    bool quality = true;
    Bytes speech; // RFC4867 sensitivity-ordered bits, MSB first, low pad bits zero
};
struct AmrPayload {
    uint8_t cmr = 15;
    std::vector<AmrFrame> frames;
};
// Baseline mono format. Optional CRC, interleaving and robust sorting are not
// negotiated or silently guessed. NB FT9..14 and WB FT10..13 are rejected.
int amrSpeechBits(AmrCodec, uint8_t frameType);
bool serializeAmr(const AmrPayload&, AmrCodec, AmrAlignment, Bytes&, std::string* error = nullptr);
bool parseAmr(const Bytes&, AmrCodec, AmrAlignment, AmrPayload&, std::string* error = nullptr);
// opencore-amr/vo-amrwbenc storage representation, one ToC octet + speech.
bool amrFromStorage(const Bytes&, AmrCodec, AmrFrame&, std::string* error = nullptr);
bool amrToStorage(const AmrFrame&, AmrCodec, Bytes&, std::string* error = nullptr);

struct RedBlock {
    uint8_t payloadType = 0;
    uint16_t timestampOffset = 0; // 14 bits; redundant blocks only
    Bytes payload;
};
struct RedPayload { std::vector<RedBlock> redundant; RedBlock primary; };
bool serializeRed(const RedPayload&, Bytes&, std::string* error = nullptr);
bool parseRed(const Bytes&, RedPayload&, std::string* error = nullptr);

struct ReceptionReport {
    uint32_t ssrc = 0;
    uint8_t fractionLost = 0;
    int32_t cumulativeLost = 0; // signed 24-bit on wire; duplicates may make it negative
    uint32_t extendedHighestSequence = 0, jitter = 0, lastSr = 0, delaySinceLastSr = 0;
};
struct SenderInfo {
    uint64_t ntpTimestamp = 0;
    uint32_t rtpTimestamp = 0, packetCount = 0, octetCount = 0;
};
struct XrBlock {
    uint8_t type = 0, typeSpecific = 0;
    Bytes data; // block body, whole 32-bit words, excludes four-byte header
};
struct RtcpPacket {
    uint8_t type = 201; // SR=200, RR=201, XR=207
    uint32_t ssrc = 0;
    SenderInfo sender;
    std::vector<ReceptionReport> reports;
    std::vector<XrBlock> xrBlocks;
};
bool serializeRtcp(const RtcpPacket&, Bytes&, std::string* error = nullptr);
// Parses a whole datagram of supported SR/RR/XR packets. These may also be
// reduced-size reports; session-level compound RTCP/SDES policy is not imposed.
bool parseRtcp(const Bytes&, std::vector<RtcpPacket>&, std::string* error = nullptr);
XrBlock makeXrReceiverReferenceTime(uint64_t ntpTimestamp);
struct DlrrSubBlock { uint32_t ssrc = 0, lastRr = 0, delaySinceLastRr = 0; };
XrBlock makeXrDlrr(const std::vector<DlrrSubBlock>&);

// RFC3550 A.1 sequence validation, A.3 report counters, A.8 jitter. Source is
// already known to this internal simulation, so initial two-packet discovery
// probation is omitted. A large sequence discontinuity still needs two packets
// to confirm a restart. Accepted duplicates count as received per RFC3550.
class ReceiverStatistics {
public:
    explicit ReceiverStatistics(uint32_t sourceSsrc = 0);
    void reset(uint32_t sourceSsrc = 0);
    bool observe(uint16_t sequence, uint32_t timestamp, uint32_t arrivalRtpUnits);
    bool observe(const RtpPacket&, double arrivalMs, uint32_t clockRate);
    void noteSenderReport(uint64_t ntpTimestamp, double receivedAtMs);
    ReceptionReport report(double nowMs); // advances interval loss baseline
    uint64_t receivedPackets() const { return received; }
    double interarrivalJitter() const { return jitterEstimate; }
private:
    uint32_t source = 0, cycles = 0, baseSequence = 0, badSequence = 65537;
    uint16_t maxSequence = 0;
    uint64_t received = 0, expectedPrior = 0, receivedPrior = 0;
    uint32_t priorTransit = 0, lastSr = 0;
    double jitterEstimate = 0, srArrivalMs = 0;
    bool initialized = false, haveTransit = false, haveSr = false;
    void initializeSequence(uint16_t);
};
}}
