#include "dsp/SignalProcessor.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <limits>
using namespace TelephonyDSP;
namespace {
int failures=0;
void check(bool ok,const char* label) { std::printf("%s: %s\n",ok?"ok":"FAIL",label); failures+=!ok; }
void set(AdvancedSettings& s,AdvancedControl c,double value) { check(s.setPlain((size_t)c,value),"valid advanced control"); }
std::vector<float> input(int size) { std::vector<float> x(size); for(int i=0;i<size;++i) x[i]=.15f*std::sin(i*.031)+.1f*std::sin(i*.103); return x; }
std::vector<float> run(SignalProcessor& dsp,const std::vector<float>& x,int block) {
    std::vector<float> out(x.size());
    for(size_t p=0;p<x.size();p+=block) { const int n=std::min<size_t>(block,x.size()-p); float* in=const_cast<float*>(x.data()+p); float* output=out.data()+p; dsp.process(&in,1,&output,1,n); }
    return out;
}
bool finiteAudio(const std::vector<float>& x) { return std::all_of(x.begin(),x.end(),[](float v){return std::isfinite(v) && std::abs(v)<8;}); }
bool same(const std::vector<float>& x,const std::vector<float>& y) { if(x.size()!=y.size())return false;for(size_t i=0;i<x.size();++i)if(std::abs(x[i]-y[i])>1e-6)return false;return true; }
double energy(const std::vector<float>& x) {double e=0;for(float v:x)e+=v*v;return e;}
}
int main() {
    using A=AdvancedControl;
    AdvancedSettings defaults;
    for(size_t i=0;i<advancedDescriptors.size();++i) check(defaults.setPlain(i,advancedDescriptors[i].initial),"default descriptor roundtrip");
    check(!defaults.setPlain(0,std::numeric_limits<double>::quiet_NaN()),"NaN rejected");
    check(!defaults.setPlain((size_t)A::DtmfDigit,.5),"fractional enum rejected");
    const auto x=input(48000);
    SignalProcessor baseline; baseline.setSampleRate(48000);baseline.setMode(EraMode::PSTN_G711);
    const auto ordinary=run(baseline,x,512);
    for(auto c:{A::DtmfDigit,A::EchoEnabled,A::FadingEnabled,A::ClockDriftPpm,A::HandoverIntervalMs,A::BitErrorRate}) {
        AdvancedSettings settings;
        double value=1;
        if(c==A::ClockDriftPpm)value=50;
        if(c==A::HandoverIntervalMs)value=200;
        if(c==A::BitErrorRate)value=.02;
        set(settings,c,value);
        SignalProcessor dsp;dsp.setSampleRate(48000);dsp.setMode(EraMode::PSTN_G711);dsp.setAdvancedSettings(settings);
        const auto a=run(dsp,x,512);
        check(finiteAudio(a),"advanced effect bounded finite output");
        check(!same(a,ordinary),"advanced control changes actual audio");
        dsp.reset(); const auto b=run(dsp,x,127);
        check(same(a,b),"advanced audio independent of host block partition and reset");
    }
    {
        auto wide=x;
        for(size_t i=0;i<wide.size();++i) wide[i]+=.1f*std::sin(2*3.141592653589793*9000*i/48000.0);
        SignalProcessor clean,tandem;
        for(auto* p:{&clean,&tandem}) {p->setSampleRate(48000);p->setMode(EraMode::EVS_LIKE);}
        AdvancedSettings setting;set(setting,A::TandemNarrowband,1);tandem.setAdvancedSettings(setting);
        const auto original=run(clean,wide,512), altered=run(tandem,wide,512);
        check(!same(original,altered)&&finiteAudio(altered),"wideband-to-narrowband tandem changes actual audio");
        tandem.reset();check(same(altered,run(tandem,wide,127)),"tandem reset and host block partition reproducible");
        std::vector<float> high(48000);for(size_t i=0;i<high.size();++i)high[i]=.2f*std::sin(2*3.141592653589793*9000*i/48000.0);
        clean.reset();tandem.reset();
        check(energy(run(tandem,high,127))<energy(run(clean,high,127))*.05,"tandem anti-alias filter rejects above-narrowband input");
    }
    AdvancedSettings network;set(network,A::NetworkEnabled,1);set(network,A::PacketFormat,1);set(network,A::JitterMs,9);set(network,A::JitterDistribution,1);set(network,A::JitterCorrelation,.7);set(network,A::PlaybackDelayMs,40);set(network,A::LossBoost,.1);set(network,A::DuplicateRate,.2);set(network,A::Redundancy,1);
    SignalProcessor dsp;dsp.setSampleRate(48000);dsp.setRoute(RouteEndpoint::FixedLine,RouteEndpoint::FixedLine,DegradationSegment::Both);dsp.setAdvancedSettings(network);
    const auto a=run(dsp,x,512);const auto metrics=dsp.getTelemetry();
    check(finiteAudio(a)&&energy(a)>1,"encoded packet queue audible");
    check(metrics.packets>50 && metrics.measuredLoss>=0 && metrics.measuredLoss<=1 && metrics.jitterMs>0,"real packet counters and jitter");
    check(dsp.getLatencySamples()==11040,"two-leg packet delay in host latency");
    dsp.reset();check(same(a,run(dsp,x,127)),"packet loss/jitter/reorder deterministic across host blocks");
    SignalProcessor dry;dry.setSampleRate(48000);dry.setRoute(RouteEndpoint::FixedLine,RouteEndpoint::FixedLine,DegradationSegment::None);dry.setAdvancedSettings(network);check(dry.getLatencySamples()==7200,"unimpaired route has no queue latency");
    AdvancedSettings tone;set(tone,A::DtmfDigit,5);
    SignalProcessor dtmf;dtmf.setSampleRate(48000);dtmf.setMode(EraMode::PSTN_G711);dtmf.setAdvancedSettings(tone);
    check(energy(run(dtmf,std::vector<float>(48000),127))>1,"DTMF test tone reaches codec with silent input");
#if TELEPHONY_EXPERIMENTAL_NETWORK
    for(int duration=0;duration<6;++duration) {
        AdvancedSettings opus;set(opus,A::OpusFrameDuration,duration);set(opus,A::NetworkEnabled,1);set(opus,A::PacketFormat,1);
        SignalProcessor p;p.setSampleRate(48000);p.setMode(EraMode::OPUS_VOIP);p.setAdvancedSettings(opus);
        const auto out=run(p,x,127);check(finiteAudio(out)&&energy(out)>1,"Opus variable duration through whole DSP");
    }
#endif
    std::printf("advanced integration failures: %d\n",failures);return failures?1:0;
}
