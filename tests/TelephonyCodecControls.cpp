#include "dsp/AmrNbCodec.h"
#include "dsp/AmrWbCodec.h"
#include "dsp/EvsCodec.h"
#include "dsp/EvsCodecJbm.h"
#include "dsp/G711Codec.h"
#include "dsp/GsmCodec.h"
#include "dsp/OpusCodec.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if TELEPHONY_EXPERIMENTAL_NETWORK
#include <opus.h>
#endif
using namespace TelephonyDSP;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"codec control line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
// CTest captures stdout through a pipe. Keep the last completed stage visible
// even if a third-party reference codec raises an OS-level exception.
static void stage(const char* name) { std::printf("[codec_controls] %s\n", name); }
#if defined(_WIN32)
static LONG WINAPI reportCodecException(EXCEPTION_POINTERS* exception) {
    const auto* record = exception->ExceptionRecord;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(stderr, "[codec_controls] Windows exception 0x%08lx at %p (image+0x%llx)\n",
        record->ExceptionCode, record->ExceptionAddress,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(record->ExceptionAddress) - base));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        std::fprintf(stderr, "[codec_controls] access type %llu, address 0x%llx\n",
            static_cast<unsigned long long>(record->ExceptionInformation[0]),
            static_cast<unsigned long long>(record->ExceptionInformation[1]));
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH; // Diagnose, never hide or handle the failure.
}
#endif
struct Capture : ICodecTransport {
    CodecPacketInfo info{CodecPacketFormat::GSM,0,0}; std::vector<uint8_t> bytes; int calls = 0;
    bool exchange(const CodecPacketInfo& i,const uint8_t* p,size_t n,bool lost,CodecPlayout& out) override {
        info=i; bytes.assign(p,p+n); ++calls;
        if(lost) return false;
        out.payload=bytes; out.dtx=i.dtx; return n>0;
    }
};
static void tone(std::vector<int16_t>& pcm,int sr,double hz,int frame=0) {
    for(size_t i=0;i<pcm.size();++i) pcm[i]=(int16_t)(9000*std::sin(6.283185307179586*hz*(i+(double)frame*pcm.size())/sr));
}
static void packetAndDtx(ICodec& codec) {
    Capture capture; codec.setTransport(&capture);
    std::vector<int16_t> input(codec.getFrameSize()),output(input.size());
    tone(input,codec.getSampleRate(),330); codec.configureDtx(false,false);
    for(int n=0;n<8;++n) codec.processFrame(input.data(),output.data(),false);
    const auto clean=capture.bytes;
    codec.configureBitErrors(1.0f); codec.processFrame(input.data(),output.data(),false);
    CHECK(codec.getInjectedBitErrors()>0); CHECK(capture.bytes.size()==clean.size());
    if(capture.info.format==CodecPacketFormat::AMRNB || capture.info.format==CodecPacketFormat::AMRWB)
        CHECK(capture.bytes[0]==clean[0]);
    if(capture.info.format==CodecPacketFormat::GSM) CHECK((capture.bytes[0]&0xf0)==0xd0);
    codec.configureBitErrors(0); codec.configureDtx(true,true); codec.setVoiceActivity(false);
    std::fill(input.begin(),input.end(),0);
    for(int n=0;n<100;++n) codec.processFrame(input.data(),output.data(),false);
    CHECK(codec.isDtxActive());
    CHECK(std::all_of(output.begin(),output.end(),[](int16_t v){return v==0;}));
    const int before=capture.calls; codec.processFrame(input.data(),output.data(),true);
    CHECK(capture.calls==before+1); CHECK(codec.isDtxActive());
    codec.setTransport(nullptr);
}
static void deterministicBer() {
    G711Codec codec(8000); Capture capture; codec.setTransport(&capture); codec.configureDtx(false,false);
    std::vector<int16_t> input(160),output(160); tone(input,8000,490);
    codec.configureBitErrors(.1f,123); codec.reset(); codec.processFrame(input.data(),output.data(),false);
    const auto first=capture.bytes; const auto flips=codec.getInjectedBitErrors();
    codec.reset(); codec.processFrame(input.data(),output.data(),false);
    CHECK(capture.bytes==first); CHECK(flips==codec.getInjectedBitErrors());
    codec.configureBitErrors(.1f,456); codec.reset(); codec.processFrame(input.data(),output.data(),false);
    CHECK(capture.bytes!=first);
}
static void amrModeContinuity() {
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    AMRNBCodec nb(false,7), freshNb(false,1); AMRWBCodec wb(false,8), freshWb(false,1);
    Capture warmCapture,freshCapture;
    std::vector<int16_t> nbIn(160),nbOut(160),wbIn(320),wbOut(320);
    tone(nbIn,8000,370); tone(wbIn,16000,370);
    nb.setTransport(&warmCapture); freshNb.setTransport(&freshCapture);
    for(int f=0;f<25;++f) nb.processFrame(nbIn.data(),nbOut.data(),false);
    nb.setMode(1); nb.processFrame(nbIn.data(),nbOut.data(),false); freshNb.processFrame(nbIn.data(),nbOut.data(),false);
    CHECK(((warmCapture.bytes[0]>>3)&15)==1); CHECK(warmCapture.bytes!=freshCapture.bytes);
    wb.setTransport(&warmCapture); freshWb.setTransport(&freshCapture);
    for(int f=0;f<25;++f) wb.processFrame(wbIn.data(),wbOut.data(),false);
    wb.setMode(1); wb.processFrame(wbIn.data(),wbOut.data(),false); freshWb.processFrame(wbIn.data(),wbOut.data(),false);
    CHECK(((warmCapture.bytes[0]>>3)&15)==1); CHECK(warmCapture.bytes!=freshCapture.bytes);
#endif
}
static void opusControls() {
#if TELEPHONY_EXPERIMENTAL_NETWORK
    OpusCodec codec(48000,24000,5); Capture capture; codec.setTransport(&capture); codec.configureDtx(false,false);
    for(float duration:{2.5f,5.f,10.f,20.f,40.f,60.f}) {
        std::printf("[codec_controls] Opus duration %.1f ms\n", duration);
        CHECK(codec.setExpertFrameDuration(duration)); CHECK(codec.getFrameSize()==(int)(48*duration));
        std::vector<int16_t> input(codec.getFrameSize()), output(input.size()); tone(input,48000,440);
        codec.processFrame(input.data(),output.data(),false);
        CHECK(opus_packet_get_nb_samples(capture.bytes.data(),(opus_int32)capture.bytes.size(),48000)==codec.getFrameSize());
        CHECK(codec.getActualMode()!=OpusMode::Unknown);
        codec.configureBitErrors(1); codec.processFrame(input.data(),output.data(),false);
        const unsigned char* frames[48]; opus_int16 sizes[48]; unsigned char toc; int offset;
        CHECK(opus_packet_parse(capture.bytes.data(),(opus_int32)capture.bytes.size(),&toc,frames,sizes,&offset)>0);
        codec.configureBitErrors(0);
    }
    CHECK(!codec.setExpertFrameDuration(15)); CHECK(codec.supportsForceMode());
    codec.setBitrate(48000); codec.setFecEnabled(false);
    for(auto mode : {OpusMode::Silk, OpusMode::Hybrid, OpusMode::Celt, OpusMode::Auto}) {
        std::printf("[codec_controls] Opus forced mode %d\n", static_cast<int>(mode));
        CHECK(codec.setForceMode(mode));
        std::vector<int16_t> input(codec.getFrameSize()),output(input.size());
        for(int f=0;f<10;++f) { tone(input,48000,1000,f); codec.processFrame(input.data(),output.data(),false); }
        if(mode!=OpusMode::Auto) CHECK(codec.getActualMode()==mode);
    }
    CHECK(codec.setExpertFrameDuration(5)); CHECK(!codec.setForceMode(OpusMode::Silk)); CHECK(!codec.setForceMode(OpusMode::Hybrid));
    CHECK(codec.setExpertFrameDuration(20)); CHECK(codec.setForceMode(OpusMode::Silk)); CHECK(!codec.setExpertFrameDuration(2.5f));
    OpusCodec fec(16000,18000,6,kOpusBandwidthWideband); fec.configureDtx(false,false);
    fec.setFecPacketLossPercent(30); fec.setPlaybackDelayFrames(2);
    std::vector<int16_t> input(320),output(320);
    for(int f=0;f<150;++f) { tone(input,16000,150+f%13,f); fec.processFrame(input.data(),output.data(),f>15&&f%5==0); }
    CHECK(fec.getFecAttempts()>0); CHECK(fec.getFecRecoveredFrames()>0);
#endif
}
static void evsControls() {
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    for(int mode=0;mode<9;++mode) {
        std::printf("[codec_controls] EVS AMR-WB IO mode %d\n", mode);
        static const int rates[]={6600,8850,12650,14250,15850,18250,19850,23050,23850};
        EVS_EncOptions opts; evs_enc_options_init(&opts); opts.amr_wb_io=1; opts.dtx_enable=1; opts.dtx_sid_interval=3;
        EVS_Encoder* enc=evs_enc_create_ex(16000,rates[mode],EVS_WB,&opts);
        EVS_Decoder* dec=evs_dec_create_ex(16000,rates[mode],1); CHECK(enc&&dec);
        std::vector<int16_t> input(320),output(320); std::array<uint8_t,6000> stream{};
        int bits=0,update=0,cmi=-1,sids=0;
        for(int f=0;f<130;++f) {
            if(f<15) tone(input,16000,280,f); else std::fill(input.begin(),input.end(),0);
            int used=0,produced=0; CHECK(evs_enc_process(enc,input.data(),320,stream.data(),(int)stream.size(),&used)==EVS_OK);
            CHECK(evs_enc_get_last_frame_info(enc,&bits,&update,&cmi)==EVS_OK);
            CHECK(cmi==mode); if(bits==35) { ++sids; CHECK(update==1); }
            CHECK(evs_dec_process(dec,stream.data(),used,output.data(),&produced)==EVS_OK); CHECK(produced==320);
            if(f==8) { CHECK(evs_dec_process_lost(dec,output.data(),&produced)==EVS_OK); CHECK(produced==320); }
        }
        CHECK(sids>0); CHECK(evs_enc_reconfigure(enc,12345,EVS_WB,&opts)==EVS_ERROR);
        evs_enc_destroy(enc); evs_dec_destroy(dec);
    }
    stage("EVS auto bandwidth and runtime profiles");
    EVSCodec dynamic(48000,16400,EVS_FB); dynamic.configureDtx(false,false); dynamic.setAutoBandwidth(true);
    std::vector<int16_t> input(960),output(960);
    for(int f=0;f<20;++f) { tone(input,48000,500,f); dynamic.processFrame(input.data(),output.data(),false); }
    CHECK(dynamic.getActiveBandwidth()==EVS_NB);
    for(int f=0;f<8;++f) { tone(input,48000,18000,f); dynamic.processFrame(input.data(),output.data(),false); }
    CHECK(dynamic.getActiveBandwidth()==EVS_FB);
    for(int bitrate:{13200,24400,32000,9600,16400,7200}) {
        CHECK(dynamic.reconfigure(bitrate,EVS_FB));
        for(int f=0;f<5;++f) { tone(input,48000,700,f); dynamic.processFrame(input.data(),output.data(),false); }
    }
    dynamic.setAutoBandwidth(false); CHECK(dynamic.setAmrWbIo(true,12650));
    for(int f=0;f<8;++f) dynamic.processFrame(input.data(),output.data(),false);
    CHECK(dynamic.setAmrWbIo(true,23850));
    Capture ioCapture; dynamic.setTransport(&ioCapture); dynamic.processFrame(input.data(),output.data(),false);
    uint16_t ioBits=0; std::memcpy(&ioBits,ioCapture.bytes.data()+2,2); CHECK(ioBits==477);
    dynamic.setTransport(nullptr);
    CHECK(dynamic.setAmrWbIo(false));
    for(int f=0;f<8;++f) dynamic.processFrame(input.data(),output.data(),false);
    stage("EVS native profile restoration");
    EVSCodec restored(32000,24400,EVS_SWB), fresh(32000,24400,EVS_SWB);
    CHECK(restored.setAmrWbIo(true,12650)); CHECK(restored.getMaxBandwidth()==EVS_SWB);
    CHECK(restored.getActiveBandwidth()==EVS_WB); CHECK(restored.setAmrWbIo(false));
    CHECK(restored.getBitrate()==24400); CHECK(restored.getMaxBandwidth()==EVS_SWB);
    restored.reset(); fresh.reset();
    std::vector<int16_t> restoredInput(640), restoredOutput(640), freshOutput(640);
    tone(restoredInput,32000,680);
    for(int f=0;f<8;++f) { restored.processFrame(restoredInput.data(),restoredOutput.data(),false); fresh.processFrame(restoredInput.data(),freshOutput.data(),false); CHECK(restoredOutput==freshOutput); }
    stage("EVS dense BER and DTX");
    EVSCodec corrupt(16000,13200,EVS_WB); packetAndDtx(corrupt);
#endif
}
static void sourceControlledProfiles() {
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    EVSCodec native(32000,24400,EVS_SWB,true);
    CHECK(native.getBitrate()==5900); native.setScVbrEnabled(false);
    CHECK(native.getBitrate()==24400); CHECK(native.getMaxBandwidth()==EVS_SWB);
    native.setScVbrEnabled(true); native.reset(); native.setScVbrEnabled(false);
    CHECK(native.getBitrate()==24400); CHECK(native.getMaxBandwidth()==EVS_SWB);
#endif
#if TELEPHONY_USE_EVS_JBM
    EVSCodecJbm initial(32000,24400,EVS_SWB,0,true), live(32000,24400,EVS_SWB);
    live.setScVbrEnabled(true); initial.reset(); live.reset();
    CHECK(initial.getBitrate()==5900); CHECK(live.getBitrate()==5900);
    std::vector<int16_t> input(640), a(640), b(640);
    for(int f=0;f<80;++f) { tone(input,32000,330,f); initial.processFrame(input.data(),a.data(),false); live.processFrame(input.data(),b.data(),false); CHECK(a==b); }
    initial.setScVbrEnabled(false); CHECK(initial.getBitrate()==24400); CHECK(initial.getMaxBandwidth()==EVS_SWB);
#endif
}
static void jbmClockDrift(int frameCount = 24000) {
#if TELEPHONY_USE_EVS_JBM
    EVSCodecJbm normal(16000,13200,EVS_WB), fast(16000,13200,EVS_WB), slow(16000,13200,EVS_WB);
    normal.configureDtx(false,false); fast.configureDtx(false,false); slow.configureDtx(false,false);
    fast.setClockDriftPpm(50); slow.setClockDriftPpm(-50);
    std::vector<int16_t> input(320),a(320),b(320),c(320); bool fastDiff=false,slowDiff=false;
    for(int f=0;f<frameCount;++f) {
        tone(input,16000,180+f%47,f);
        normal.processFrame(input.data(),a.data(),false); fast.processFrame(input.data(),b.data(),false); slow.processFrame(input.data(),c.data(),false);
        fastDiff |= a!=b; slowDiff |= a!=c;
    }
    CHECK(std::abs(fast.getReceiverClockSkewMs()-frameCount*.001)<.08);
    CHECK(std::abs(slow.getReceiverClockSkewMs()+frameCount*.001)<.08);
    CHECK(fastDiff); CHECK(slowDiff);
#endif
}
static void bitErrorStress() {
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    // The reference decoder must survive dense valid-symbol corruption,
    // including speech/SID transitions and changing core/bandwidth modes.
    for (int bitrate : {7200,8000,9600,13200,16400,24400,32000,48000,64000,96000,128000}) {
        std::printf("[codec_controls] EVS BER bitrate %d\n", bitrate);
        EVSCodec codec(48000,bitrate,EVS_FB); codec.configureBitErrors(.05f,(uint32_t)bitrate);
        std::vector<int16_t> input(960), output(960);
        for(int f=0;f<80;++f) { tone(input,48000,120+(f*77)%15000,f); codec.processFrame(input.data(),output.data(),f%19==0); }
    }
#endif
#if TELEPHONY_USE_EVS_JBM
    EVSCodecJbm jbm(32000,13200,EVS_SWB); Capture capture; jbm.setTransport(&capture);
    std::vector<int16_t> input(640),output(640); jbm.configureBitErrors(.01f);
    for(int f=0;f<80;++f) { tone(input,32000,330,f); jbm.processFrame(input.data(),output.data(),f%13==0); }
    CHECK(capture.calls==80); CHECK(jbm.getInjectedBitErrors()>0);
    jbm.configureBitErrors(0); jbm.setAutoBandwidth(true); jbm.configureDtx(false,false);
    for(int f=0;f<15;++f) { tone(input,32000,500,f); jbm.processFrame(input.data(),output.data(),false); }
    CHECK(jbm.getActiveBandwidth()==EVS_NB); CHECK(jbm.reconfigure(16400,EVS_SWB));
    jbm.configureDtx(true,true); jbm.setVoiceActivity(false); std::fill(input.begin(),input.end(),0);
    for(int f=0;f<100;++f) jbm.processFrame(input.data(),output.data(),false);
    CHECK(jbm.isDtxActive()); CHECK(std::all_of(output.begin(),output.end(),[](int16_t v){return v==0;}));
#endif
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#if defined(_WIN32)
    SetUnhandledExceptionFilter(reportCodecException);
#endif
    if(argc>1 && std::string(argv[1])=="--jbm-short") { jbmClockDrift(1000); return 0; }
    if(argc>1 && std::string(argv[1])=="--jbm-clock") { jbmClockDrift(); return 0; }
    if(argc>1 && std::string(argv[1])=="--evs-only") { evsControls(); bitErrorStress(); return 0; }
    if(argc>1 && std::string(argv[1])=="--opus-only") { opusControls(); return 0; }
    stage("G.711 packet, BER and DTX"); G711Codec g711(8000); packetAndDtx(g711);
    stage("GSM packet, BER and DTX"); GSMCodec gsm; packetAndDtx(gsm);
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    stage("AMR-NB packet, BER and DTX"); AMRNBCodec nb; packetAndDtx(nb);
    stage("AMR-WB packet, BER and DTX"); AMRWBCodec wb; packetAndDtx(wb);
#endif
    stage("Deterministic BER"); deterministicBer();
    stage("AMR mode continuity"); amrModeContinuity();
    stage("Opus controls"); opusControls();
    stage("EVS controls"); evsControls();
    stage("Source-controlled profiles"); sourceControlledProfiles();
    stage("BER stress"); bitErrorStress();
    std::puts("Codec control regressions passed");
}
