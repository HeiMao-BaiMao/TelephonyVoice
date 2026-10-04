// Synthetic-only clock-domain regression. GNU link wrapping observes the
// unmodified 3GPP reference APA implementation, not a substitute time scaler.
#include "dsp/EvsCodecJbm.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace TelephonyDSP;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"JBM drift line %d: %s\n",__LINE__,#c); return 1; } } while(0)
struct ApaStats { uint64_t nonUnityRequests=0, stretched=0, shrunk=0; };
static ApaStats* observing = nullptr;
#if TELEPHONY_TEST_APA_WRAP
extern "C" {
struct apa_state_t;
unsigned char __real_apa_set_scale(apa_state_t*,uint16_t);
uint8_t __real_apa_exec(apa_state_t*,const int16_t*,uint16_t,uint16_t,int16_t*,uint16_t*);
unsigned char __wrap_apa_set_scale(apa_state_t* state,uint16_t scale) {
    if(observing && scale!=100) ++observing->nonUnityRequests;
    return __real_apa_set_scale(state,scale);
}
uint8_t __wrap_apa_exec(apa_state_t* state,const int16_t* in,uint16_t count,uint16_t maxScaling,int16_t* out,uint16_t* produced) {
    const auto result=__real_apa_exec(state,in,count,maxScaling,out,produced);
    if(observing && result==0) {
        if(*produced>count) ++observing->stretched;
        if(*produced<count) ++observing->shrunk;
    }
    return result;
}
}
#endif
int main() {
    constexpr int frames=64000, baselineFrames=1000, rate=16000, frameSize=320;
    EVSCodecJbm zero(rate,13200,EVS_WB), positive(rate,13200,EVS_WB), negative(rate,13200,EVS_WB);
    zero.configureDtx(false,false); positive.configureDtx(false,false); negative.configureDtx(false,false);
    positive.setClockDriftPpm(50); negative.setClockDriftPpm(-50);
    std::vector<int16_t> input(frameSize),a(frameSize),b(frameSize),c(frameSize);
    ApaStats baseline,fast,slow; bool positiveDifferent=false,negativeDifferent=false;
    for(int f=0;f<frames;++f) {
        for(int i=0;i<frameSize;++i) input[(size_t)i]=(int16_t)(9000*std::sin(6.283185307179586*(180+f%47)*(i+(double)f*frameSize)/rate));
        if(f<baselineFrames) { observing=f>=200?&baseline:nullptr; zero.processFrame(input.data(),a.data(),false); }
        observing=f>=200?&fast:nullptr; positive.processFrame(input.data(),b.data(),false);
        observing=f>=200?&slow:nullptr; negative.processFrame(input.data(),c.data(),false);
        if(f<baselineFrames) { positiveDifferent |= a!=b; negativeDifferent |= a!=c; }
    }
    observing=nullptr;
    const uint64_t inputSamples=(uint64_t)frames*frameSize;
    const auto expectedPositive=(uint64_t)std::floor(inputSamples/(1.0+50e-6));
    const auto expectedNegative=(uint64_t)std::floor(inputSamples/(1.0-50e-6));
    CHECK(zero.getGeneratedSourceSamples()==(uint64_t)baselineFrames*frameSize);
    CHECK(positive.getGeneratedSourceSamples()==expectedPositive);
    CHECK(negative.getGeneratedSourceSamples()==expectedNegative);
    CHECK(positive.getEncodedFrameCount()==expectedPositive/frameSize);
    CHECK(negative.getEncodedFrameCount()==expectedNegative/frameSize);
    CHECK(positive.getEncodedFrameCount()<(uint64_t)frames);
    CHECK(negative.getEncodedFrameCount()>(uint64_t)frames);
    CHECK(positiveDifferent && negativeDifferent);
    std::printf("Source packets (zero baseline runs 1000 frames): zero=%llu +50ppm=%llu -50ppm=%llu; APA steady-state stretch/shrink: zero=%llu/%llu positive=%llu/%llu negative=%llu/%llu\n",
        (unsigned long long)zero.getEncodedFrameCount(),(unsigned long long)positive.getEncodedFrameCount(),(unsigned long long)negative.getEncodedFrameCount(),
        (unsigned long long)baseline.stretched,(unsigned long long)baseline.shrunk,
        (unsigned long long)fast.stretched,(unsigned long long)fast.shrunk,
        (unsigned long long)slow.stretched,(unsigned long long)slow.shrunk);
#if TELEPHONY_TEST_APA_WRAP
    CHECK(fast.nonUnityRequests>0 && slow.nonUnityRequests>0);
    CHECK(fast.stretched>baseline.stretched);
    CHECK(slow.shrunk>baseline.shrunk);
#endif
    return 0;
}
