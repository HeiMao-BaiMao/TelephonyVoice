#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// RTP payload syntax: 3GPP TS 26.445 V19.1.0, Annex A.2.1--A.2.3/A.2.5:
// https://www.etsi.org/deliver/etsi_ts/126400_126499/126445/19.01.00_60/ts_126445v190100p.pdf
// AMR-WB IO bit ordering and SID fields: TS 26.201 V19.0.0, 4.2 and Annex B:
// https://www.etsi.org/deliver/etsi_ts/126200_126299/126201/19.00.00_60/ts_126201v190000p.pdf
// This module serializes payload bytes only. RTP headers/padding, SDP negotiation,
// codec operation, and the RFC 4867 backward-compatible format are separate layers.
// The session layer applies negotiated mode/rate limits, packetization duration,
// and NO_DATA suppression recommendations; the syntax layer can represent them.
namespace TelephonyDSP::Transport {

enum class EvsMode { Primary, AmrWbIo };
enum class EvsPayloadFormat { Auto, Compact, HeaderFull };
enum class EvsG192ByteOrder { Native, LittleEndian, BigEndian };

struct EvsFrame {
    EvsMode mode = EvsMode::Primary;
    std::uint8_t frameType = 15; // Four-bit rate index; 14=lost, 15=no data.
    bool quality = true;        // AMR-WB IO Q only. Primary has no Q bit.
    // MSB-first, natural encoder ordering for Primary, IF1 sensitivity ordering
    // for AMR-WB IO. The latter includes all 40 SID bits, including STI and CMI.
    std::vector<std::uint8_t> data;
};

struct EvsPayloadOptions {
    bool headerFullOnly = false; // Negotiated hf-only=1; disables size detection.
    EvsPayloadFormat format = EvsPayloadFormat::Auto; // Serializer selection.
    // Full CMR byte (H=1); 0xff is NO_REQ. Empty means no requested change.
    // Auto uses a compact 3-bit CMR when this request is exactly representable.
    std::optional<std::uint8_t> cmr;
    std::size_t channels = 1; // Frame order: chronological blocks, channel order.
    std::size_t maxFrames = 256; // Resource limit, not a codec/spec limit.
};

struct EvsPayload {
    EvsPayloadFormat format = EvsPayloadFormat::HeaderFull;
    // Compact IO CMR is expanded to a full byte. Reserved incoming CMR values
    // are retained; applications must ignore them (see evsCmrIsDefined).
    std::optional<std::uint8_t> cmr;
    std::vector<EvsFrame> frames;
};

// Returns -1 for reserved/invalid frame types, otherwise the coded payload bits.
int evsFrameBits(EvsMode mode, std::uint8_t frameType) noexcept;
bool evsCmrIsDefined(std::uint8_t cmr) noexcept;
bool evsProtectedPayloadSize(std::size_t bytes) noexcept;

// On false, output is unchanged; error, if supplied, describes the failure.
// Serialization emits zero padding and rejects noncanonical input frame padding.
// Parsing ignores padding bit values as required by the specification, but checks
// lengths, header chains, required CMRs, reserved FTs, and channel grouping.
// Parse options.format/cmr are ignored: negotiated headerFullOnly and wire size
// determine the format. Never try Header-Full first and guess on parse failure.
bool serializeEvsPayload(const std::vector<EvsFrame>& frames,
                         std::vector<std::uint8_t>& output,
                         const EvsPayloadOptions& options = {},
                         std::string* error = nullptr);
bool parseEvsPayload(const std::uint8_t* data, std::size_t size, EvsPayload& output,
                     const EvsPayloadOptions& options = {},
                     std::string* error = nullptr);
inline bool parseEvsPayload(const std::vector<std::uint8_t>& data, EvsPayload& output,
                            const EvsPayloadOptions& options = {},
                            std::string* error = nullptr) {
    return parseEvsPayload(data.data(), data.size(), output, options, error);
}

struct EvsAmrWbSid {
    bool update = true;          // STI: false=SID_FIRST, true=SID_UPDATE.
    std::uint8_t codecMode = 0;  // CMI 0..8, not present in G.192.
};
struct EvsG192Options {
    // Explicit mode prevents ambiguous zero-length frames from being guessed.
    EvsMode mode = EvsMode::Primary;
    EvsG192ByteOrder byteOrder = EvsG192ByteOrder::Native;
    // Required for IO's 35-bit SID; optional SID_FIRST hint for a good empty
    // G.192 frame. A good empty IO frame without this hint means NO_DATA.
    std::optional<EvsAmrWbSid> sid;
};
struct EvsG192Frame {
    std::vector<std::uint8_t> data;
    EvsMode mode = EvsMode::Primary;
    // Side information retained because G.192 alone cannot represent STI/CMI.
    std::optional<EvsAmrWbSid> sid;
};

// evs_api.c/fx.c use native-endian 16-bit words: 0x6b21/0x6b20 sync,
// count, then 0x007f/0x0081 bits. Native is the default; no aligned casts.
// Primary BAD frames become SPEECH_LOST (there is no Primary Q bit).
// IO conversion performs the TS 26.201 permutation, not just bit packing.
// IO SID_FIRST becomes a good zero-bit G.192 frame plus explicit side info.
// An IO SPEECH_LOST frame reconstructs with Q=0 (its Q has no speech data).
// Transport conversion does not imply that evs_api's decoder supports IO mode.
bool evsFrameFromG192(const std::uint8_t* data, std::size_t size, EvsFrame& output,
                      const EvsG192Options& options = {},
                      std::string* error = nullptr);
inline bool evsFrameFromG192(const std::vector<std::uint8_t>& data, EvsFrame& output,
                             const EvsG192Options& options = {},
                             std::string* error = nullptr) {
    return evsFrameFromG192(data.data(), data.size(), output, options, error);
}
bool evsFrameToG192(const EvsFrame& frame, EvsG192Frame& output,
                    EvsG192ByteOrder byteOrder = EvsG192ByteOrder::Native,
                    std::string* error = nullptr);

} // namespace TelephonyDSP::Transport
