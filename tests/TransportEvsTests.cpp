#include "dsp/TransportEvs.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace TelephonyDSP::Transport;
namespace {
int checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": " #expr " failed\n"; std::exit(1); \
} } while (false)

EvsFrame makeFrame(EvsMode mode, std::uint8_t ft) {
    EvsFrame f;
    f.mode = mode;
    f.frameType = ft;
    const int bits = evsFrameBits(mode, ft);
    CHECK(bits >= 0);
    f.data.resize(static_cast<std::size_t>((bits + 7) / 8));
    for (std::size_t j = 0; j < f.data.size(); ++j)
        f.data[j] = static_cast<std::uint8_t>((j * 137 + ft * 13 + 0xa7) & 255);
    if (bits % 8) f.data.back() &= static_cast<std::uint8_t>(0xffu << (8 - bits % 8));
    if (mode == EvsMode::Primary && ft == 0) f.data[0] &= 0x7f;
    if (mode == EvsMode::AmrWbIo && ft == 9) f.data[4] = static_cast<std::uint8_t>((f.data[4] & 0xe0) | 0x18);
    return f;
}
bool sameFrame(const EvsFrame& a, const EvsFrame& b) {
    return a.mode == b.mode && a.frameType == b.frameType &&
           a.quality == b.quality && a.data == b.data;
}
void checkRoundtrip(const std::vector<EvsFrame>& frames, EvsPayloadOptions opts) {
    std::vector<std::uint8_t> bytes;
    EvsPayload parsed;
    std::string error = "stale";
    CHECK(serializeEvsPayload(frames, bytes, opts, &error));
    CHECK(error.empty());
    CHECK(parseEvsPayload(bytes, parsed, opts, &error));
    CHECK(error.empty());
    CHECK(parsed.frames.size() == frames.size());
    for (std::size_t j = 0; j < frames.size(); ++j) CHECK(sameFrame(frames[j], parsed.frames[j]));
    std::vector<std::uint8_t> again;
    opts.cmr = parsed.cmr;
    CHECK(serializeEvsPayload(parsed.frames, again, opts));
    CHECK(again == bytes);
}
void lengthsAndModes() {
    const int primary[] = {56,144,160,192,264,328,488,640,960,1280,1920,2560,48,-1,0,0};
    const int io[] = {132,177,253,285,317,365,397,461,477,40,-1,-1,-1,-1,0,0};
    const std::size_t protectedSizes[] = {6,7,17,18,20,23,24,32,33,36,40,41,46,50,58,60,61,80,120,160,240,320};
    for (unsigned ft = 0; ft < 16; ++ft) {
        CHECK(evsFrameBits(EvsMode::Primary, static_cast<std::uint8_t>(ft)) == primary[ft]);
        CHECK(evsFrameBits(EvsMode::AmrWbIo, static_cast<std::uint8_t>(ft)) == io[ft]);
    }
    CHECK(evsFrameBits(static_cast<EvsMode>(42), 0) == -1);
    CHECK(evsFrameBits(EvsMode::Primary, 16) == -1);
    for (std::size_t size = 0; size <= 322; ++size)
        CHECK(evsProtectedPayloadSize(size) ==
              (std::find(std::begin(protectedSizes), std::end(protectedSizes), size) != std::end(protectedSizes)));
    for (auto mode : {EvsMode::Primary, EvsMode::AmrWbIo}) {
        for (std::uint8_t ft = 0; ft < 16; ++ft) {
            if (evsFrameBits(mode, ft) < 0) continue;
            const auto frame = makeFrame(mode, ft);
            checkRoundtrip({frame}, {});
            EvsPayloadOptions hf;
            hf.headerFullOnly = true;
            checkRoundtrip({frame}, hf);
            hf.headerFullOnly = false;
            hf.cmr = 0xff;
            checkRoundtrip({frame}, hf);
            if (mode == EvsMode::AmrWbIo) {
                auto bad = frame;
                bad.quality = false;
                checkRoundtrip({bad}, {});
            }
        }
    }
}
void goldenPayloads() {
    // Independent wire fixtures from Annex A's bit diagrams, not a roundtrip.
    EvsFrame primary;
    primary.frameType = 12;
    primary.data = {0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    std::vector<std::uint8_t> bytes;
    CHECK(serializeEvsPayload({primary}, bytes));
    CHECK(bytes == primary.data);
    EvsPayloadOptions hf;
    hf.headerFullOnly = true;
    CHECK(serializeEvsPayload({primary}, bytes, hf));
    CHECK(bytes == std::vector<std::uint8_t>({0x0c,0xaa,0xbb,0xcc,0xdd,0xee,0xff}));
    // The same 7 bytes in default mode mean Primary 2.8, never guessed HF SID.
    EvsPayload parsed;
    CHECK(parseEvsPayload(bytes, parsed));
    CHECK(parsed.format == EvsPayloadFormat::Compact);
    CHECK(parsed.frames[0].frameType == 0);
    CHECK(parseEvsPayload(bytes, parsed, hf));
    CHECK(parsed.frames[0].frameType == 12);
    hf.headerFullOnly = false;
    hf.cmr = 0xff;
    CHECK(serializeEvsPayload({primary}, bytes, hf));
    CHECK(bytes == std::vector<std::uint8_t>({0xff,0x0c,0xaa,0xbb,0xcc,0xdd,0xee,0xff}));

    auto io = makeFrame(EvsMode::AmrWbIo, 0);
    io.data.assign(17,0);
    io.data[0] = 0xc0; // d(0)=d(1)=1; 130 zero bits follow.
    CHECK(serializeEvsPayload({io}, bytes));
    CHECK(bytes.size() == 17 && bytes[0] == 0xf0 && bytes[16] == 0x02);
    CHECK(std::all_of(bytes.begin()+1,bytes.end()-1,[](std::uint8_t b){return b==0;}));
    CHECK(parseEvsPayload(bytes, parsed));
    CHECK(sameFrame(io, parsed.frames[0]));
    CHECK(parsed.cmr == 0xff);
    // Padding is ignored on receive, including the compact IO final bit.
    bytes[16] |= 1;
    CHECK(parseEvsPayload(bytes, parsed));
    CHECK(sameFrame(io, parsed.frames[0]));

    auto sid = makeFrame(EvsMode::AmrWbIo,9);
    sid.data = {0x11,0x22,0x33,0x44,0x15};
    CHECK(serializeEvsPayload({sid},bytes));
    CHECK(bytes == std::vector<std::uint8_t>({0xff,0x39,0x11,0x22,0x33,0x44,0x15}));
    CHECK(parseEvsPayload(bytes,parsed));
    CHECK(parsed.format == EvsPayloadFormat::HeaderFull);
    CHECK(sameFrame(sid,parsed.frames[0]));
    sid.quality=false;
    CHECK(serializeEvsPayload({sid},bytes));
    CHECK(bytes[1]==0x29 && bytes.size()==7);

    // Primary 8 kbps HF+CMR is 22 bytes. 7.2 HF+CMR collides at 20,
    // requiring one padding octet. hf-only does not apply collision padding.
    auto f72=makeFrame(EvsMode::Primary,1);
    CHECK(serializeEvsPayload({f72},bytes,hf));
    CHECK(bytes.size()==21 && bytes[0]==0xff && bytes[1]==1 && bytes.back()==0);
    checkRoundtrip({f72},hf);
    auto io660=makeFrame(EvsMode::AmrWbIo,0);
    hf.headerFullOnly=true;
    CHECK(serializeEvsPayload({io660},bytes,hf));
    CHECK(bytes.size()==19 && bytes[0]==0xff && bytes[1]==0x30);
    bytes.back() |= 0x0f; // IO per-frame byte-alignment bits are ignored.
    CHECK(parseEvsPayload(bytes,parsed,hf));
    CHECK(sameFrame(io660,parsed.frames[0]));
}
void cmrAndChannels() {
    // Exhaustive independent Table A.3 validity expectations.
    for (unsigned c = 0; c <= 255; ++c) {
        const unsigned t=(c>>4)&7,d=c&15;
        const bool expected=(c&128) && ((t==0&&d<=6)||(t==1&&d<=8)||
            (t==2&&d<=11)||(t==3&&d>=3&&d<=11)||(t==4&&d>=5&&d<=11)||
            ((t==5||t==6)&&d<=7)||(t==7&&d==15));
        CHECK(evsCmrIsDefined(static_cast<std::uint8_t>(c))==expected);
    }
    const std::array<std::uint8_t,8> cmrs={0x90,0x91,0x92,0x94,0x95,0x97,0x98,0xff};
    for (std::size_t i=0;i<cmrs.size();++i) {
        EvsPayloadOptions options;
        options.cmr=cmrs[i];
        auto frame=makeFrame(EvsMode::AmrWbIo,0);
        std::vector<std::uint8_t> bytes;
        CHECK(serializeEvsPayload({frame},bytes,options));
        CHECK(bytes[0]>>5==i);
        checkRoundtrip({frame},options);
    }
    for (auto cmr : {0x93,0x96,0xa4,0xd7,0xe4}) {
        EvsPayloadOptions options;
        options.cmr=static_cast<std::uint8_t>(cmr);
        std::vector<std::uint8_t> bytes;
        CHECK(serializeEvsPayload({makeFrame(EvsMode::AmrWbIo,1)},bytes,options));
        EvsPayload parsed;
        CHECK(parseEvsPayload(bytes,parsed));
        CHECK(parsed.format==EvsPayloadFormat::HeaderFull && parsed.cmr==cmr);
    }
    std::vector<EvsFrame> frames={makeFrame(EvsMode::Primary,4),makeFrame(EvsMode::AmrWbIo,1),
                                 makeFrame(EvsMode::Primary,15),makeFrame(EvsMode::Primary,12)};
    EvsPayloadOptions stereo;
    stereo.channels=2;
    checkRoundtrip(frames,stereo);
    std::vector<std::uint8_t> bytes;
    CHECK(serializeEvsPayload(frames,bytes,stereo));
    CHECK(bytes[0]==0xff && bytes[1]==0x44 && bytes[2]==0x71 && bytes[3]==0x4f && bytes[4]==0x0c);
    frames.pop_back();
    CHECK(!serializeEvsPayload(frames,bytes,stereo));
    stereo.channels=3;
    EvsPayload parsed;
    CHECK(!parseEvsPayload(bytes,parsed,stereo));
}
void malformed() {
    EvsPayloadOptions hf;
    hf.headerFullOnly=true;
    EvsPayload sentinel;
    sentinel.cmr=0x95;
    sentinel.frames={makeFrame(EvsMode::Primary,4)};
    const auto old=sentinel.frames[0];
    std::string error;
    for (const auto& bytes : std::vector<std::vector<std::uint8_t>>{
        {},{0xff},{0x4f},{0xff,0x80},{0x0d},{0x1f},{0x3a},{0x20},
        {0x04,0},{0x0f,0},{0xff,0xff,0x0f}}) {
        CHECK(!parseEvsPayload(bytes,sentinel,hf,&error));
        CHECK(!error.empty());
        CHECK(sentinel.cmr==0x95 && sentinel.frames.size()==1 && sameFrame(sentinel.frames[0],old));
    }
    CHECK(!parseEvsPayload(nullptr,5,sentinel,hf));
    EvsPayloadOptions tiny=hf;
    tiny.maxFrames=1;
    CHECK(!parseEvsPayload(std::vector<std::uint8_t>{0x4f,0x0f},sentinel,tiny));
    // Seven-byte MSB=1 must be exactly CMR + IO SID, not arbitrary HF.
    CHECK(!parseEvsPayload(std::vector<std::uint8_t>{0xff,0x0f,0,0,0,0,0},sentinel));
    // Incoming unknown CMR preserved for upper layer to ignore.
    CHECK(parseEvsPayload(std::vector<std::uint8_t>{0xf0,0x0f},sentinel,hf));
    CHECK(sentinel.cmr==0xf0 && !evsCmrIsDefined(*sentinel.cmr));

    std::vector<std::uint8_t> output={42};
    auto invalid=makeFrame(EvsMode::Primary,4);
    invalid.data.pop_back();
    CHECK(!serializeEvsPayload({invalid},output));
    CHECK(output==std::vector<std::uint8_t>{42});
    CHECK(!serializeEvsPayload({},output));
    invalid=makeFrame(EvsMode::Primary,0);
    invalid.data[0]|=0x80;
    CHECK(!serializeEvsPayload({invalid},output));
    invalid=makeFrame(EvsMode::AmrWbIo,1);
    invalid.data.back()|=1;
    CHECK(!serializeEvsPayload({invalid},output));
    invalid=makeFrame(EvsMode::Primary,4);
    invalid.quality=false;
    CHECK(!serializeEvsPayload({invalid},output));
    EvsPayloadOptions opts;
    opts.cmr=0xf0;
    CHECK(!serializeEvsPayload({makeFrame(EvsMode::Primary,4)},output,opts));
    opts={}; opts.format=EvsPayloadFormat::HeaderFull;
    CHECK(!serializeEvsPayload({makeFrame(EvsMode::Primary,4)},output,opts));
    opts={}; opts.format=EvsPayloadFormat::Compact;
    CHECK(!serializeEvsPayload({makeFrame(EvsMode::AmrWbIo,9)},output,opts));
    opts.headerFullOnly=true;
    CHECK(!serializeEvsPayload({makeFrame(EvsMode::Primary,4)},output,opts));
    opts={}; opts.channels=0;
    CHECK(!serializeEvsPayload({makeFrame(EvsMode::Primary,4)},output,opts));
}
void g192() {
    for (auto order : {EvsG192ByteOrder::Native,EvsG192ByteOrder::LittleEndian,EvsG192ByteOrder::BigEndian}) {
        for (auto mode : {EvsMode::Primary,EvsMode::AmrWbIo}) {
            for (std::uint8_t ft=0;ft<16;++ft) {
                if (evsFrameBits(mode,ft)<0) continue;
                auto frame=makeFrame(mode,ft);
                if (mode==EvsMode::AmrWbIo && ft==14) frame.quality=false;
                EvsG192Frame wire;
                CHECK(evsFrameToG192(frame,wire,order));
                EvsG192Options opts;
                opts.mode=mode;opts.byteOrder=order;opts.sid=wire.sid;
                EvsFrame restored;
                CHECK(evsFrameFromG192(wire.data,restored,opts));
                CHECK(sameFrame(frame,restored));
                // Deliberately unaligned storage: evs_api passes byte buffers.
                auto unaligned=wire.data;
                unaligned.insert(unaligned.begin(),0x7f);
                CHECK(evsFrameFromG192(unaligned.data()+1,unaligned.size()-1,restored,opts));
                CHECK(sameFrame(frame,restored));
            }
        }
    }
    auto primary=makeFrame(EvsMode::Primary,12);
    primary.data={0xa0,0,0,0,0,1};
    EvsG192Frame stream;
    CHECK(evsFrameToG192(primary,stream,EvsG192ByteOrder::LittleEndian));
    CHECK(std::vector<std::uint8_t>(stream.data.begin(),stream.data.begin()+10)==
          std::vector<std::uint8_t>({0x21,0x6b,48,0,0x81,0,0x7f,0,0x81,0}));
    EvsG192Options opts;opts.byteOrder=EvsG192ByteOrder::LittleEndian;
    EvsFrame decoded;
    auto bad=stream.data;
    bad[0]=0x20;
    CHECK(evsFrameFromG192(bad,decoded,opts));
    CHECK(decoded.frameType==14 && decoded.data.empty());
    bad=stream.data;bad[4]=0x80;
    CHECK(!evsFrameFromG192(bad,decoded,opts));
    bad=stream.data;bad.push_back(0);bad.push_back(0);
    CHECK(!evsFrameFromG192(bad,decoded,opts));
    bad=stream.data;bad[2]=47;
    CHECK(!evsFrameFromG192(bad,decoded,opts));
    bad=stream.data;bad[0]=0;
    CHECK(!evsFrameFromG192(bad,decoded,opts));
    CHECK(!evsFrameFromG192(nullptr,0,decoded,opts));

    // Independent natural-order/IF1 fixture: TS 26.201 table B.1 starts 0,5,6,7.
    std::vector<std::uint8_t> ioNatural(4+2*132,0);
    ioNatural[0]=0x21;ioNatural[1]=0x6b;ioNatural[2]=132;
    for (std::size_t i=0;i<132;++i) ioNatural[4+2*i]=0x7f;
    ioNatural[4]=ioNatural[4+2*5]=0x81;
    opts.mode=EvsMode::AmrWbIo;
    CHECK(evsFrameFromG192(ioNatural,decoded,opts));
    CHECK(decoded.frameType==0 && decoded.data[0]==0xc0);
    CHECK(std::all_of(decoded.data.begin()+1,decoded.data.end(),[](std::uint8_t b){return b==0;}));
    CHECK(evsFrameToG192(decoded,stream,EvsG192ByteOrder::LittleEndian));
    CHECK(stream.data==ioNatural);

    auto sid=makeFrame(EvsMode::AmrWbIo,9);
    CHECK(evsFrameToG192(sid,stream,EvsG192ByteOrder::LittleEndian));
    CHECK(stream.data.size()==74 && stream.sid && stream.sid->codecMode==8 && stream.sid->update);
    CHECK(!evsFrameFromG192(stream.data,decoded,opts)); // CMI must never be guessed.
    opts.sid=stream.sid;
    CHECK(evsFrameFromG192(stream.data,decoded,opts));
    CHECK(sameFrame(sid,decoded));
    sid.data={0,0,0,0,3}; // SID_FIRST with CMI 3.
    CHECK(evsFrameToG192(sid,stream,EvsG192ByteOrder::LittleEndian));
    CHECK(stream.data==std::vector<std::uint8_t>({0x21,0x6b,0,0}));
    CHECK(stream.sid && !stream.sid->update && stream.sid->codecMode==3);
    opts.sid=stream.sid;
    CHECK(evsFrameFromG192(stream.data,decoded,opts));
    CHECK(sameFrame(sid,decoded));
    opts.sid.reset();
    CHECK(evsFrameFromG192(stream.data,decoded,opts));
    CHECK(decoded.frameType==15); // Same G.192 bytes; explicit metadata disambiguates.
    sid.data[4]=0x1f;
    CHECK(!evsFrameToG192(sid,stream));
}
void randomized() {
    std::mt19937 random(0x26445);
    for (int trial=0;trial<600;++trial) {
        std::vector<EvsFrame> frames;
        const auto count=1+random()%8;
        for (unsigned i=0;i<count;++i) {
            const auto mode=random()%2 ? EvsMode::Primary : EvsMode::AmrWbIo;
            const auto ft=static_cast<std::uint8_t>(random()%(mode==EvsMode::Primary?13:10));
            auto frame=makeFrame(mode,ft);
            for (auto& byte : frame.data) byte=static_cast<std::uint8_t>(random());
            const int bits=evsFrameBits(mode,ft);
            if (bits%8) frame.data.back()&=static_cast<std::uint8_t>(0xffu<<(8-bits%8));
            if (mode==EvsMode::Primary && ft==0) frame.data[0]&=0x7f;
            frames.push_back(std::move(frame));
        }
        EvsPayloadOptions options;
        options.headerFullOnly=(trial%2)==0;
        checkRoundtrip(frames,options);
    }
    // Bounded parser robustness corpus. Run under ASan/UBSan as well.
    EvsPayload output;
    for (int trial=0;trial<4000;++trial) {
        std::vector<std::uint8_t> bytes(random()%500);
        for (auto& byte : bytes) byte=static_cast<std::uint8_t>(random());
        EvsPayloadOptions options;
        options.headerFullOnly=(trial%2)==0;
        (void)parseEvsPayload(bytes,output,options);
    }
}
}
int main() {
    lengthsAndModes();goldenPayloads();cmrAndChannels();malformed();g192();randomized();
    std::cout<<"EVS transport: "<<checks<<" checks passed\n";
}
