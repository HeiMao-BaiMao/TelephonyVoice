#include "dsp/SignalProcessor.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace TelephonyDSP;
using A=AdvancedControl;
static int failures=0;
static void check(bool ok,const char* label) { std::printf("%s: %s\n",ok?"ok":"FAIL",label);failures+=!ok; }
static void set(AdvancedSettings& s,A key,double value) { if(!s.setPlain((size_t)key,value)) std::abort(); }
static std::vector<float> signal(int n) { std::vector<float>x(n); for(int i=0;i<n;++i)x[i]=.12f*std::sin(i*.107f)+.05f*std::sin(i*1.113f);return x; }
static std::vector<float> render(SignalProcessor& d,const std::vector<float>& x,int block=127) { std::vector<float> y(x.size()); for(size_t p=0;p<x.size();p+=block) { int n=std::min<size_t>(block,x.size()-p); float* in=const_cast<float*>(x.data()+p); float* out=y.data()+p;d.process(&in,1,&out,1,n); }return y; }
static bool equal(const std::vector<float>& a,const std::vector<float>& b) { if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();++i)if(!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::abs(a[i]-b[i])>1e-6)return false;return true; }
static double energy(const std::vector<float>& x,size_t first,size_t last) { double e=0;for(size_t i=first;i<std::min(last,x.size());++i)e+=double(x[i])*x[i];return e; }
int main() {
    const auto x=signal(48000);
    {
        AdvancedSettings net;set(net,A::NetworkEnabled,1);set(net,A::PlaybackDelayMs,100);
        SignalProcessor d;d.setSampleRate(48000);d.setRoute(RouteEndpoint::FixedLine,RouteEndpoint::FixedLine,DegradationSegment::None);d.setAdvancedSettings(net);d.setParameters(0,0,false,0);
        render(d,x);
        d.setRoute(RouteEndpoint::FixedLine,RouteEndpoint::FixedLine,DegradationSegment::Both);
        auto changed=render(d,x);
        SignalProcessor fresh;fresh.setSampleRate(48000);fresh.setRoute(RouteEndpoint::FixedLine,RouteEndpoint::FixedLine,DegradationSegment::Both);fresh.setAdvancedSettings(net);fresh.setParameters(0,0,false,0);
        const auto expected=render(fresh,x);
        check(equal(changed,expected),"segment-only route latency change re-primes dry timeline");
        std::printf("segment latency=%d initial energy=%g\n",d.getLatencySamples(),energy(changed,0,d.getLatencySamples()));
    }
#if TELEPHONY_EXPERIMENTAL_NETWORK
    {
        AdvancedSettings silk;set(silk,A::NetworkEnabled,1);set(silk,A::OpusForceMode,1);
        AdvancedSettings shortAuto=silk;set(shortAuto,A::OpusForceMode,0);set(shortAuto,A::OpusFrameDuration,0);
        SignalProcessor changed;changed.setSampleRate(48000);changed.setMode(EraMode::OPUS_VOIP);changed.setAdvancedSettings(silk);render(changed,x);
        changed.setAdvancedSettings(shortAuto);changed.reset();const auto a=render(changed,x);
        SignalProcessor fresh;fresh.setSampleRate(48000);fresh.setMode(EraMode::OPUS_VOIP);fresh.setAdvancedSettings(shortAuto);const auto b=render(fresh,x);
        std::printf("Opus changed packets=%llu fresh packets=%llu\n",(unsigned long long)changed.getTelemetry().packets,(unsigned long long)fresh.getTelemetry().packets);
        check(equal(a,b),"Silk-to-short-Auto control transition realizes requested frame duration");
    }
#endif
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    {
        AdvancedSettings io;set(io,A::EvsAmrWbIo,1);
        SignalProcessor changed;changed.setSampleRate(48000);changed.setMode(EraMode::EVS_NATIVE);changed.setEVSConfig(32000,24400,EVS_SWB);render(changed,x);
        changed.setAdvancedSettings(io);render(changed,x);
        set(io,A::EvsAmrWbIo,0);changed.setAdvancedSettings(io);changed.reset();const auto a=render(changed,x);
        SignalProcessor fresh;fresh.setSampleRate(48000);fresh.setMode(EraMode::EVS_NATIVE);fresh.setEVSConfig(32000,24400,EVS_SWB);const auto b=render(fresh,x);
        check(equal(a,b),"EVS IO exit restores configured native bitrate and bandwidth");
    }
#endif
    {
        AdvancedSettings settings;set(settings,A::NetworkEnabled,1);set(settings,A::PlaybackDelayMs,0);set(settings,A::PsdNoise,1);
        SignalProcessor d;d.setSampleRate(48000);d.setMode(EraMode::PSTN_G711);d.setAdvancedSettings(settings);
        auto renderPsd=[&]() {
            d.setParameters(1,0,false,0,0,0);d.reset();render(d,x);
            d.setParameters(1,0,false,0,.95f,0);return render(d,x);
        };
        const auto first=renderPsd(),second=renderPsd();
        const bool bounded=std::all_of(first.begin(),first.end(),[](float sample){return std::isfinite(sample) && std::abs(sample)<2;});
        check(d.getTelemetry().lost>0 && energy(first,0,first.size())>1 && bounded,"PSD concealment is audible and bounded under independent-network loss");
        check(equal(first,second),"PSD concealment and transport reset reproducibly");
        set(settings,A::PsdNoise,0);d.setAdvancedSettings(settings);const auto legacy=renderPsd();
        check(!equal(first,legacy),"PSD control selects a distinct packet-loss concealment model");
    }
    {
        auto speech=signal(16000);std::fill(speech.begin()+4000,speech.end(),0.f);
        auto dtxRender=[&](bool network) {
            AdvancedSettings settings;set(settings,A::DtxEnabled,1);set(settings,A::PureSilence,1);set(settings,A::NetworkEnabled,network?1:0);set(settings,A::PlaybackDelayMs,100);
            SignalProcessor d;d.setSampleRate(8000);d.setMode(EraMode::PSTN_G711);d.setAdvancedSettings(settings);
            auto audio=render(d,speech);audio.erase(audio.begin(),audio.begin()+d.getLatencySamples());audio.resize(13000);return audio;
        };
        const auto baseline=dtxRender(false),queued=dtxRender(true);
        check(equal(baseline,queued),"lossless packet queue preserves DTX speech/silence alignment");
        double difference=0;for(size_t i=0;i<baseline.size();++i)difference+=std::abs(baseline[i]-queued[i]);std::printf("DTX queue aligned absolute difference=%g\n",difference);
    }
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    for(auto mode:{EraMode::AMR_NB_3G,EraMode::AMR_WB_VOLTE,EraMode::EVS_NATIVE}) {
        AdvancedSettings adaptive;set(adaptive,A::FadingEnabled,1);set(adaptive,A::CarrierToInterferenceDb,-10);set(adaptive,A::AdaptiveBitrate,1);
        SignalProcessor changed;changed.setSampleRate(48000);changed.setMode(mode);changed.setAdvancedSettings(adaptive);render(changed,x);
        set(adaptive,A::AdaptiveBitrate,0);changed.setAdvancedSettings(adaptive);changed.reset();const auto a=render(changed,x);
        SignalProcessor fresh;fresh.setSampleRate(48000);fresh.setMode(mode);fresh.setAdvancedSettings(adaptive);const auto b=render(fresh,x);
        char label[100];std::snprintf(label,sizeof(label),"disabling adaptive bitrate restores user codec mode (%d)",(int)mode);check(equal(a,b),label);
    }
#endif
    {
        AdvancedSettings drift;set(drift,A::ClockDriftPpm,50);
        SignalProcessor changed;changed.setSampleRate(48000);changed.setMode(EraMode::PSTN_G711);changed.setParameters(.5,0,false,0);changed.setAdvancedSettings(drift);render(changed,x);
        set(drift,A::ClockDriftPpm,-50);changed.setAdvancedSettings(drift);const auto a=render(changed,x);
        SignalProcessor fresh;fresh.setSampleRate(48000);fresh.setMode(EraMode::PSTN_G711);fresh.setParameters(.5,0,false,0);fresh.setAdvancedSettings(drift);const auto b=render(fresh,x);
        check(equal(a,b),"same-latency framing change re-primes the complete dry/wet timeline");
    }
#if TELEPHONY_EXPERIMENTAL_NETWORK
    {
        AdvancedSettings all;set(all,A::NetworkEnabled,1);set(all,A::PlaybackDelayMs,100);set(all,A::JitterMs,9);set(all,A::LossBoost,.1);set(all,A::ClockDriftPpm,50);set(all,A::EchoEnabled,1);set(all,A::FadingEnabled,1);set(all,A::HandoverIntervalMs,200);set(all,A::TandemNarrowband,1);set(all,A::OpusFrameDuration,5);
        SignalProcessor combined;combined.setSampleRate(48000);combined.setRoute(RouteEndpoint::Opus,RouteEndpoint::Opus,DegradationSegment::Both);combined.setAdvancedSettings(all);const auto a=render(combined,x,512);combined.reset();const auto b=render(combined,x,7);
        check(equal(a,b),"two long-frame Opus legs with combined impairments are block independent");
    }
#endif
    {
        AdvancedSettings network;set(network,A::NetworkEnabled,1);set(network,A::PlaybackDelayMs,100);
        SignalProcessor d;d.setSampleRate(384000);d.setRoute(RouteEndpoint::FixedLine,RouteEndpoint::FixedLine,DegradationSegment::Both);d.setAdvancedSettings(network);d.setParameters(0,0,false,0);
        const auto input=signal(384000);const auto output=render(d,input,4096);const size_t latency=d.getLatencySamples();bool exact=true;
        for(size_t i=0;i<output.size();++i)if(output[i]!=(i<latency?0:input[i-latency])) { exact=false;break; }
        check(exact,"maximum-rate two-leg packet delay retains exact dry latency without ring overflow");
    }
#if TELEPHONY_EXPERIMENTAL_NETWORK
    for(int duration:{3,5}) {
        AdvancedSettings settings;set(settings,A::OpusFrameDuration,duration);
        SignalProcessor d;d.setSampleRate(48000);d.setMode(EraMode::OPUS_VOIP);d.setAdvancedSettings(settings);
        const auto output=render(d,x);size_t first=0;while(first<output.size() && std::abs(output[first])<.001f)++first;
        const int frameSamples=duration==3?960:2880;
        check(first>=size_t(d.getLatencySamples()) && first-size_t(d.getLatencySamples())<size_t(frameSamples),"legacy Opus playout delay is included in reported latency");
    }
#endif
    std::printf("advanced edge failures: %d\n",failures);return failures?1:0;
}
