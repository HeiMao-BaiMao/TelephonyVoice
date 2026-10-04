#include "dsp/Transport.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace TelephonyDSP { namespace Transport {
namespace {
constexpr std::size_t maxWireBytes = 262144;
bool fail(std::string* error, const char* message) { if (error) *error = message; return false; }
void put16(Bytes& b, uint16_t x) { b.push_back(static_cast<uint8_t>(x >> 8)); b.push_back(static_cast<uint8_t>(x)); }
void put32(Bytes& b, uint32_t x) { put16(b, static_cast<uint16_t>(x >> 16)); put16(b, static_cast<uint16_t>(x)); }
uint16_t get16(const Bytes& b, std::size_t i) { return static_cast<uint16_t>((b[i] << 8) | b[i + 1]); }
uint32_t get32(const Bytes& b, std::size_t i) { return (uint32_t(get16(b, i)) << 16) | get16(b, i + 2); }
int64_t signed32(uint32_t x) { return x <= 0x7fffffffu ? int64_t(x) : int64_t(x) - 0x100000000ll; }
bool zeroPadding(const Bytes& b, int bits) {
    if (bits == 0 || bits % 8 == 0) return true;
    return (b.back() & ((1u << (8 - bits % 8)) - 1)) == 0;
}
struct BitWriter {
    Bytes data;
    std::size_t bit = 0;
    void put(uint32_t value, unsigned n) {
        for (unsigned i = n; i > 0; --i) {
            if (bit % 8 == 0) data.push_back(0);
            data.back() |= uint8_t(((value >> (i - 1)) & 1) << (7 - bit % 8)); ++bit;
        }
    }
};
struct BitReader {
    const Bytes& data;
    std::size_t bit = 0;
    bool get(unsigned n, uint32_t& value) {
        if (n > 32 || n > data.size() * 8 - bit) return false;
        value = 0;
        while (n--) { value = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1); ++bit; }
        return true;
    }
};
bool validFrame(const AmrFrame& f, AmrCodec codec) {
    const int bits = amrSpeechBits(codec, f.frameType);
    return bits >= 0 && f.speech.size() == std::size_t((bits + 7) / 8) && zeroPadding(f.speech, bits);
}
void putReport(Bytes& b, const ReceptionReport& r) {
    put32(b, r.ssrc); b.push_back(r.fractionLost);
    const auto loss = uint32_t(std::clamp<int32_t>(r.cumulativeLost, -8388608, 8388607)) & 0xffffff;
    b.push_back(uint8_t(loss >> 16)); put16(b, uint16_t(loss));
    put32(b, r.extendedHighestSequence); put32(b, r.jitter); put32(b, r.lastSr); put32(b, r.delaySinceLastSr);
}
ReceptionReport getReport(const Bytes& b, std::size_t p) {
    ReceptionReport r;
    r.ssrc = get32(b, p); r.fractionLost = b[p + 4];
    const uint32_t lost = (uint32_t(b[p + 5]) << 16) | get16(b, p + 6);
    r.cumulativeLost = lost & 0x800000 ? int32_t(int64_t(lost) - 0x1000000) : int32_t(lost);
    r.extendedHighestSequence = get32(b, p + 8); r.jitter = get32(b, p + 12);
    r.lastSr = get32(b, p + 16); r.delaySinceLastSr = get32(b, p + 20); return r;
}
bool validXrBlock(const XrBlock& block) {
    if (block.data.size() % 4 || block.data.size() / 4 > 65535) return false;
    if (block.type == 4 && block.data.size() != 8) return false;
    if (block.type == 5 && block.data.size() % 12) return false;
    if (block.type == 6 && block.data.size() != 36) return false;
    if (block.type == 7 && block.data.size() != 32) return false;
    return true;
}
}
bool serializeRtp(const RtpPacket& p, Bytes& output, std::string* error) {
    if (p.payloadType > 127 || p.csrcs.size() > 15) return fail(error, "RTP PT/CSRC count out of range");
    if (p.extension.size() % 4 || p.extension.size() / 4 > 65535 || (!p.hasExtension && !p.extension.empty()))
        return fail(error, "Invalid RTP extension length or X flag");
    if (p.payload.size() + p.extension.size() + 76 + p.padding > maxWireBytes)
        return fail(error, "RTP packet exceeds internal size limit");
    Bytes b; b.reserve(12 + p.csrcs.size() * 4 + p.extension.size() + 4 + p.payload.size() + p.padding);
    b.push_back(uint8_t(0x80 | (p.padding ? 0x20 : 0) | (p.hasExtension ? 0x10 : 0) | p.csrcs.size()));
    b.push_back(uint8_t((p.marker ? 0x80 : 0) | p.payloadType));
    put16(b, p.sequence); put32(b, p.timestamp); put32(b, p.ssrc);
    for (auto c : p.csrcs) put32(b, c);
    if (p.hasExtension) { put16(b, p.extensionProfile); put16(b, uint16_t(p.extension.size() / 4)); b.insert(b.end(), p.extension.begin(), p.extension.end()); }
    b.insert(b.end(), p.payload.begin(), p.payload.end());
    if (p.padding) { b.insert(b.end(), p.padding, 0); b.back() = p.padding; }
    output = std::move(b); if (error) error->clear(); return true;
}
bool parseRtp(const Bytes& b, RtpPacket& output, std::string* error) {
    if (b.size() < 12 || b.size() > maxWireBytes || (b[0] >> 6) != 2) return fail(error, "Invalid RTP header/version");
    RtpPacket p; p.marker = (b[1] & 0x80) != 0; p.payloadType = b[1] & 127;
    p.sequence = get16(b, 2); p.timestamp = get32(b, 4); p.ssrc = get32(b, 8);
    std::size_t pos = 12, end = b.size();
    const auto cc = b[0] & 15;
    if (std::size_t(cc) * 4 > end - pos) return fail(error, "Truncated RTP CSRC list");
    for (int i = 0; i < cc; ++i, pos += 4) p.csrcs.push_back(get32(b, pos));
    p.hasExtension = (b[0] & 0x10) != 0;
    if (p.hasExtension) {
        if (end - pos < 4) return fail(error, "Truncated RTP extension header");
        p.extensionProfile = get16(b, pos); const std::size_t bytes = std::size_t(get16(b, pos + 2)) * 4; pos += 4;
        if (bytes > end - pos) return fail(error, "Truncated RTP extension data");
        p.extension.assign(b.begin() + pos, b.begin() + pos + bytes); pos += bytes;
    }
    if (b[0] & 0x20) {
        p.padding = b.back();
        if (p.padding == 0 || p.padding > end - pos) return fail(error, "Invalid RTP padding");
        end -= p.padding;
    }
    p.payload.assign(b.begin() + pos, b.begin() + end); output = std::move(p);
    if (error) error->clear();
    return true;
}
bool sequenceBefore(uint16_t a, uint16_t b) { return a != b && uint16_t(b - a) < 0x8000; }
int amrSpeechBits(AmrCodec codec, uint8_t ft) {
    static constexpr int nb[] = {95,103,118,134,148,159,204,244,39,-1,-1,-1,-1,-1,-1,0};
    static constexpr int wb[] = {132,177,253,285,317,365,397,461,477,40,-1,-1,-1,-1,0,0};
    return ft < 16 ? (codec == AmrCodec::Narrowband ? nb[ft] : wb[ft]) : -1;
}
bool serializeAmr(const AmrPayload& p, AmrCodec codec, AmrAlignment alignment, Bytes& output, std::string* error) {
    if ((p.cmr != 15 && p.cmr > (codec == AmrCodec::Narrowband ? 7 : 8)) || p.frames.empty() || p.frames.size() > 256)
        return fail(error, "Invalid AMR CMR/frame count");
    for (const auto& f : p.frames) if (!validFrame(f, codec)) return fail(error, "Invalid AMR frame type, size or padding");
    BitWriter w;
    w.put(p.cmr, 4); if (alignment == AmrAlignment::OctetAligned) w.put(0, 4);
    for (std::size_t i = 0; i < p.frames.size(); ++i) {
        w.put(i + 1 < p.frames.size() ? 1 : 0, 1); w.put(p.frames[i].frameType, 4); w.put(p.frames[i].quality ? 1 : 0, 1);
        if (alignment == AmrAlignment::OctetAligned) w.put(0, 2);
    }
    for (const auto& f : p.frames) {
        int bits = amrSpeechBits(codec, f.frameType);
        if (alignment == AmrAlignment::OctetAligned) bits = int(f.speech.size() * 8);
        for (int i = 0; i < bits; ++i) w.put((f.speech[std::size_t(i) / 8] >> (7 - i % 8)) & 1, 1);
    }
    output = std::move(w.data); if (error) error->clear(); return true;
}
bool parseAmr(const Bytes& b, AmrCodec codec, AmrAlignment alignment, AmrPayload& output, std::string* error) {
    if (b.empty() || b.size() > maxWireBytes) return fail(error, "Empty/oversize AMR payload");
    BitReader reader{b}; AmrPayload p; uint32_t value = 0;
    reader.get(4, value); p.cmr = uint8_t(value);
    if (p.cmr != 15 && p.cmr > (codec == AmrCodec::Narrowband ? 7 : 8)) p.cmr = 15;
    // RFC4867: reserved/padding bits MUST be ignored by the receiver.
    if (alignment == AmrAlignment::OctetAligned && !reader.get(4, value)) return fail(error, "Truncated AMR header");
    bool follows = true;
    while (follows) {
        if (p.frames.size() == 256 || !reader.get(1, value)) return fail(error, "Truncated/oversize AMR ToC");
        follows = value != 0; AmrFrame f;
        if (!reader.get(4, value)) return fail(error, "Truncated AMR FT");
        f.frameType = uint8_t(value);
        if (amrSpeechBits(codec, f.frameType) < 0) return fail(error, "Reserved AMR frame type");
        if (!reader.get(1, value)) return fail(error, "Truncated AMR quality bit");
        f.quality = value != 0;
        if (alignment == AmrAlignment::OctetAligned && !reader.get(2, value)) return fail(error, "Truncated AMR ToC padding");
        p.frames.push_back(std::move(f));
    }
    for (auto& f : p.frames) {
        const int bits = amrSpeechBits(codec, f.frameType); f.speech.assign(std::size_t((bits + 7) / 8), 0);
        for (int i = 0; i < bits; ++i) {
            if (!reader.get(1, value)) return fail(error, "Truncated AMR speech bits");
            f.speech[std::size_t(i) / 8] |= uint8_t(value << (7 - i % 8));
        }
        if (alignment == AmrAlignment::OctetAligned && bits % 8 && !reader.get(8 - bits % 8, value))
            return fail(error, "Truncated AMR speech padding");
    }
    const std::size_t padding = b.size() * 8 - reader.bit;
    if (padding > 7 || (padding && !reader.get(unsigned(padding), value))) return fail(error, "Extra AMR data");
    output = std::move(p); if (error) error->clear(); return true;
}
bool amrFromStorage(const Bytes& b, AmrCodec codec, AmrFrame& out, std::string* error) {
    if (b.empty()) return fail(error, "Missing AMR storage ToC");
    AmrFrame f; f.frameType = (b[0] >> 3) & 15; f.quality = (b[0] & 4) != 0;
    f.speech.assign(b.begin() + 1, b.end());
    const int bits = amrSpeechBits(codec, f.frameType);
    if (bits > 0 && bits % 8 && f.speech.size() == std::size_t((bits + 7) / 8))
        f.speech.back() &= uint8_t(0xff << (8 - bits % 8));
    if (!validFrame(f, codec)) return fail(error, "Invalid AMR storage frame");
    out = std::move(f); if (error) error->clear(); return true;
}
bool amrToStorage(const AmrFrame& f, AmrCodec codec, Bytes& out, std::string* error) {
    if (!validFrame(f, codec)) return fail(error, "Invalid AMR storage frame");
    Bytes b{uint8_t((f.frameType << 3) | (f.quality ? 4 : 0))}; b.insert(b.end(), f.speech.begin(), f.speech.end());
    out = std::move(b); if (error) error->clear(); return true;
}
bool serializeRed(const RedPayload& p, Bytes& out, std::string* error) {
    if (p.primary.payloadType > 127 || p.primary.timestampOffset != 0 || p.redundant.size() > 256)
        return fail(error, "Invalid RED primary type/offset or block count");
    std::size_t total = 1 + p.redundant.size() * 4 + p.primary.payload.size();
    for (const auto& r : p.redundant) {
        if (r.payloadType > 127 || r.timestampOffset > 16383 || r.payload.size() > 1023) return fail(error, "RED block fields overflow");
        total += r.payload.size();
    }
    if (total > maxWireBytes) return fail(error, "RED payload too large");
    Bytes b; b.reserve(total);
    for (const auto& r : p.redundant) {
        b.push_back(uint8_t(0x80 | r.payloadType));
        b.push_back(uint8_t(r.timestampOffset >> 6));
        b.push_back(uint8_t((r.timestampOffset << 2) | (r.payload.size() >> 8))); b.push_back(uint8_t(r.payload.size()));
    }
    b.push_back(p.primary.payloadType);
    for (const auto& r : p.redundant) b.insert(b.end(), r.payload.begin(), r.payload.end());
    b.insert(b.end(), p.primary.payload.begin(), p.primary.payload.end());
    out = std::move(b); if (error) error->clear(); return true;
}
bool parseRed(const Bytes& b, RedPayload& output, std::string* error) {
    if (b.empty() || b.size() > maxWireBytes) return fail(error, "Empty/oversize RED payload");
    RedPayload p; std::vector<std::size_t> lengths; std::size_t pos = 0;
    while (pos < b.size() && (b[pos] & 0x80)) {
        if (b.size() - pos < 4 || lengths.size() == 256) return fail(error, "Truncated/oversize RED block headers");
        RedBlock r; r.payloadType = b[pos] & 127; r.timestampOffset = uint16_t((uint16_t(b[pos + 1]) << 6) | (b[pos + 2] >> 2));
        lengths.push_back(((b[pos + 2] & 3) << 8) | b[pos + 3]); p.redundant.push_back(std::move(r)); pos += 4;
    }
    if (pos == b.size()) return fail(error, "Missing RED primary header");
    p.primary.payloadType = b[pos++];
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        if (lengths[i] > b.size() - pos) return fail(error, "Truncated RED redundant block");
        p.redundant[i].payload.assign(b.begin() + pos, b.begin() + pos + lengths[i]); pos += lengths[i];
    }
    p.primary.payload.assign(b.begin() + pos, b.end()); output = std::move(p);
    if (error) error->clear();
    return true;
}
bool serializeRtcp(const RtcpPacket& p, Bytes& output, std::string* error) {
    if (p.type != 200 && p.type != 201 && p.type != 207) return fail(error, "Unsupported RTCP packet type");
    if (p.reports.size() > 31 || (p.type == 207 && !p.reports.empty()) || (p.type != 207 && !p.xrBlocks.empty()))
        return fail(error, "Invalid RTCP report fields/count");
    Bytes b{uint8_t(0x80 | (p.type == 207 ? 0 : p.reports.size())), p.type, 0, 0}; put32(b, p.ssrc);
    if (p.type == 200) {
        put32(b, uint32_t(p.sender.ntpTimestamp >> 32)); put32(b, uint32_t(p.sender.ntpTimestamp));
        put32(b, p.sender.rtpTimestamp); put32(b, p.sender.packetCount); put32(b, p.sender.octetCount);
    }
    for (const auto& r : p.reports) putReport(b, r);
    for (const auto& block : p.xrBlocks) {
        if (!validXrBlock(block) || b.size() + 4 + block.data.size() > maxWireBytes) return fail(error, "Invalid RTCP XR block/length");
        b.push_back(block.type); b.push_back(block.typeSpecific); put16(b, uint16_t(block.data.size() / 4));
        b.insert(b.end(), block.data.begin(), block.data.end());
    }
    if (b.size() > maxWireBytes) return fail(error, "RTCP packet too large");
    const auto words = uint16_t(b.size() / 4 - 1); b[2] = uint8_t(words >> 8); b[3] = uint8_t(words);
    output = std::move(b); if (error) error->clear(); return true;
}
bool parseRtcp(const Bytes& b, std::vector<RtcpPacket>& output, std::string* error) {
    if (b.empty() || b.size() > maxWireBytes || b.size() % 4) return fail(error, "Invalid RTCP datagram size");
    std::vector<RtcpPacket> result; std::size_t pos = 0;
    while (pos < b.size()) {
        if (b.size() - pos < 8 || (b[pos] >> 6) != 2) return fail(error, "Invalid RTCP header/version");
        const std::size_t size = (std::size_t(get16(b, pos + 2)) + 1) * 4;
        if (size < 8 || size > b.size() - pos) return fail(error, "Truncated RTCP packet");
        std::size_t end = pos + size;
        if (b[pos] & 0x20) {
            const std::size_t padding = b[end - 1];
            if (end != b.size() || !padding || padding % 4 || padding > size - 8) return fail(error, "Invalid RTCP padding");
            end -= padding;
        }
        RtcpPacket p; p.type = b[pos + 1]; p.ssrc = get32(b, pos + 4); const unsigned count = b[pos] & 31;
        std::size_t at = pos + 8;
        if (p.type == 200) {
            if (end - at < 20) return fail(error, "Truncated RTCP sender info");
            p.sender.ntpTimestamp = (uint64_t(get32(b, at)) << 32) | get32(b, at + 4);
            p.sender.rtpTimestamp = get32(b, at + 8); p.sender.packetCount = get32(b, at + 12); p.sender.octetCount = get32(b, at + 16); at += 20;
        } else if (p.type != 201 && p.type != 207) return fail(error, "Unsupported RTCP packet type");
        if (p.type == 207) {
            // XR's low five header bits are reserved and ignored on reception.
            while (at < end) {
                if (end - at < 4) return fail(error, "Truncated RTCP XR header");
                XrBlock block; block.type = b[at]; block.typeSpecific = b[at + 1];
                const std::size_t bytes = std::size_t(get16(b, at + 2)) * 4; at += 4;
                if (bytes > end - at) return fail(error, "Truncated RTCP XR block");
                block.data.assign(b.begin() + at, b.begin() + at + bytes); at += bytes;
                if (!validXrBlock(block)) return fail(error, "Invalid known RTCP XR block length");
                p.xrBlocks.push_back(std::move(block));
            }
        } else {
            if (end - at != count * 24) return fail(error, "RTCP report count/length mismatch");
            for (unsigned i = 0; i < count; ++i, at += 24) p.reports.push_back(getReport(b, at));
        }
        result.push_back(std::move(p)); pos += size;
    }
    output = std::move(result); if (error) error->clear(); return true;
}
XrBlock makeXrReceiverReferenceTime(uint64_t ntp) {
    XrBlock block; block.type = 4; put32(block.data, uint32_t(ntp >> 32)); put32(block.data, uint32_t(ntp)); return block;
}
XrBlock makeXrDlrr(const std::vector<DlrrSubBlock>& entries) {
    XrBlock block; block.type = 5;
    for (const auto& e : entries) { put32(block.data, e.ssrc); put32(block.data, e.lastRr); put32(block.data, e.delaySinceLastRr); }
    return block;
}
ReceiverStatistics::ReceiverStatistics(uint32_t ssrc) : source(ssrc) {}
void ReceiverStatistics::reset(uint32_t ssrc) { *this = ReceiverStatistics(ssrc); }
void ReceiverStatistics::initializeSequence(uint16_t sequence) {
    maxSequence = sequence; baseSequence = sequence; cycles = 0; badSequence = 65537;
    received = expectedPrior = receivedPrior = 0; initialized = true; haveTransit = false; jitterEstimate = 0;
}
bool ReceiverStatistics::observe(uint16_t sequence, uint32_t timestamp, uint32_t arrival) {
    if (!initialized) initializeSequence(sequence);
    else {
        const uint16_t delta = uint16_t(sequence - maxSequence);
        if (delta < 3000) { if (sequence < maxSequence) cycles += 65536; maxSequence = sequence; }
        else if (delta <= 65536 - 100) {
            if (sequence == badSequence) initializeSequence(sequence);
            else { badSequence = (uint32_t(sequence) + 1) & 65535; return false; }
        }
    }
    ++received;
    const uint32_t transit = arrival - timestamp;
    if (haveTransit) jitterEstimate += (std::abs(double(signed32(transit - priorTransit))) - jitterEstimate) / 16.0;
    priorTransit = transit; haveTransit = true; return true;
}
bool ReceiverStatistics::observe(const RtpPacket& p, double arrivalMs, uint32_t rate) {
    if (!rate || !std::isfinite(arrivalMs) || p.ssrc != source) return false;
    double ticks = std::fmod(arrivalMs * (double(rate) / 1000), 4294967296.0);
    if (!std::isfinite(ticks)) return false;
    if (ticks < 0) ticks += 4294967296.0;
    return observe(p.sequence, p.timestamp, uint32_t(ticks));
}
void ReceiverStatistics::noteSenderReport(uint64_t ntp, double timeMs) {
    if (!std::isfinite(timeMs)) return;
    lastSr = uint32_t(ntp >> 16); srArrivalMs = timeMs; haveSr = true;
}
ReceptionReport ReceiverStatistics::report(double nowMs) {
    ReceptionReport r; r.ssrc = source;
    const uint64_t expected = initialized ? uint64_t(cycles) + maxSequence - baseSequence + 1 : 0;
    const int64_t lost = int64_t(expected) - int64_t(received);
    r.cumulativeLost = int32_t(std::clamp<int64_t>(lost, -8388608, 8388607));
    const uint64_t expectedInterval = expected - expectedPrior, receivedInterval = received - receivedPrior;
    const int64_t lostInterval = int64_t(expectedInterval) - int64_t(receivedInterval);
    if (expectedInterval && lostInterval > 0) r.fractionLost = uint8_t(std::min<uint64_t>(255, uint64_t(lostInterval) * 256 / expectedInterval));
    expectedPrior = expected; receivedPrior = received;
    r.extendedHighestSequence = initialized ? cycles + maxSequence : 0;
    r.jitter = uint32_t(std::clamp(jitterEstimate, 0.0, double(std::numeric_limits<uint32_t>::max())));
    if (haveSr) {
        r.lastSr = lastSr;
        if (std::isfinite(nowMs) && nowMs >= srArrivalMs)
            r.delaySinceLastSr = uint32_t(std::min((nowMs - srArrivalMs) * 65.536, double(std::numeric_limits<uint32_t>::max())));
    }
    return r;
}
}}
