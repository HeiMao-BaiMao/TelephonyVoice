#include "dsp/SpeexDSPAux.h"
#include <cmath>
#if TELEPHONY_EXPERIMENTAL_NETWORK
#include <speex/speex_preprocess.h>
#endif

namespace TelephonyDSP {

 // ---------------------------------------------------------------------------
 // SpeexDSPAux
 // ---------------------------------------------------------------------------
#if TELEPHONY_EXPERIMENTAL_NETWORK
 SpeexDSPAux::SpeexDSPAux()
 : state(nullptr), sampleRate(0), frameSize(0), configured(false),
 vadEnabled(false), energyVadEnabled(false),
 energyVadThreshold(500.0f), lastEnergyVadProb(0.0f) {}

 SpeexDSPAux::~SpeexDSPAux() {
 if (state) speex_preprocess_state_destroy(static_cast<SpeexPreprocessState*>(state));
 state = nullptr;
 }

 void SpeexDSPAux::configure(int sr, int fs) {
 sampleRate = sr;
 frameSize = fs;
 if (state) {
 speex_preprocess_state_destroy(static_cast<SpeexPreprocessState*>(state));
 state = nullptr;
 }
 SpeexPreprocessState* s = speex_preprocess_state_init(frameSize, sampleRate);
 if (!s) {
 configured = false;
 vadEnabled = false;
 return;
 }
 int denoise =1;
 speex_preprocess_ctl(s, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
 // AGC intentionally disabled: SPEEX_PREPROCESS_SET_AGC_LEVEL takes a
 // float (not an int level). Conservative behavior keeps denoise only;
 // callers can opt-in to AGC later via a float-based API.
 //
 // VAD intentionally disabled: SpeexDSP's VAD is a placeholder
 // ("warning: The VAD has been replaced by a hack pending a complete
 // rewrite") and SpeexDSPAux is currently a foundation/placeholder
 // that does not drive audio behavior. Re-enable here (and plumb the
 // probability into the audio path) once a proper VAD story lands.
 // When re-enabling, flip vadEnabled = true here as well so
 // getSpeechProbability() / lastFrameIsSpeech() start returning
 // Speex-reported values instead of the "unknown" sentinel.
 // speex_preprocess_ctl(s, SPEEX_PREPROCESS_SET_VAD, &vad);
 vadEnabled = false;
 state = s;
 configured = true;
 // Enable the local energy-based VAD by default so callers that ask
 // for getSpeechProbability() / lastFrameIsSpeech() get a meaningful
 // answer instead of the "-1 = unknown" sentinel. This replaces the
 // disabled upstream SpeexDSP VAD (see the SpeexDSPAux header comment).
 // Callers that want the legacy "unknown" behavior can opt out with
 // setEnergyVadEnabled(false).
 energyVadEnabled = true;
 // Auto-scale the energy-VAD threshold by sqrt(frameSize/160). Frame
 // RMS grows like sqrt(N) for white noise and like sqrt(N) for a sine
 // (since the mean of |sin| over a full period is2/pi, roughly
 // constant), so dividing by sqrt(N) keeps the threshold invariant
 // across frame sizes and avoids penalizing long frames.160 is the
 // reference size used by8 kHz /20 ms Opus-style frames.
 if (frameSize >0) {
 const float kRefFrame =160.0f;
 energyVadThreshold =500.0f * std::sqrt(static_cast<float>(frameSize) / kRefFrame);
 } else {
 energyVadThreshold =500.0f;
 }
 // Flush the cached probability so a caller that asks before any
 // frame is processed still gets "unknown"-style zeros rather than
 // stale data from a previous configure() cycle.
 lastEnergyVadProb =0.0f;
 }

 void SpeexDSPAux::reset() {
 if (!state || !configured) return;
 if (sampleRate >0 && frameSize >0) {
 // SPEEX_PREPROCESS_RESET_STATE doesn't exist in this SpeexDSP
 // build; recreate the state to flush internal buffers.
 configure(sampleRate, frameSize);
 }
 // Freshly-processed frames have not happened yet; make sure callers
 // see the "unknown" sentinel from getSpeechProbability() until
 // runPreprocess() repopulates it.
 lastEnergyVadProb =0.0f;
 }

 void SpeexDSPAux::setEnergyVadEnabled(bool enable, float threshold) {
 energyVadEnabled = enable;
 if (threshold >0.0f) {
 energyVadThreshold = threshold;
 }
 // No-op for `state`: the energy VAD is computed locally on the
 // post-denoise frame in runPreprocess() and does not require any
 // SpeexDSP state. The threshold argument lets callers tune for a
 // specific mic level; configure() will still overwrite it with the
 // sqrt(frameSize/160)-scaled default on the next call.
 }

 float SpeexDSPAux::getSpeechProbability() const {
 if (!state || !configured) return -1.0f;
 // Energy VAD takes precedence: it replaces the disabled upstream
 // SpeexDSP VAD. When it's off we fall through to the legacy branch
 // (which still returns -1.0f because SpeexDSP's VAD ctl is off).
 if (energyVadEnabled) return lastEnergyVadProb;
 if (!vadEnabled) return -1.0f;
 // SPEEX_PREPROCESS_GET_PROB returns speech probability as spx_int32_t
 // (percent, [0,100]). SPEEX_PREPROCESS_GET_PSD is the power spectrum,
 // not speech probability, so it can't be returned as a probability.
 spx_int32_t prob = -1;
 speex_preprocess_ctl(static_cast<SpeexPreprocessState*>(state), SPEEX_PREPROCESS_GET_PROB, &prob);
 if (prob <0) return -1.0f;
 return static_cast<float>(prob) /100.0f;
 }

 bool SpeexDSPAux::lastFrameIsSpeech() const {
 if (!state || !configured) return false;
 if (energyVadEnabled) return lastEnergyVadProb >0.5f;
 // VAD is disabled by design (see configure()); treat the result as
 // "unknown" rather than reporting a false negative.
 if (!vadEnabled) return false;
 int vad =0;
 speex_preprocess_ctl(static_cast<SpeexPreprocessState*>(state), SPEEX_PREPROCESS_GET_VAD, &vad);
 return vad !=0;
 }

 void SpeexDSPAux::runPreprocess(int16_t* frame) {
 if (!state || !configured || !frame) return;
 speex_preprocess_run(static_cast<SpeexPreprocessState*>(state), (spx_int16_t*)frame);
 if (!energyVadEnabled) {
 // Without the energy VAD we leave lastEnergyVadProb untouched; it
 // still holds the value from the last run() that had it enabled
 // (or0.0f after configure/reset), which matches the legacy
 // "unknown"-style behavior of getSpeechProbability().
 return;
 }
 // Energy VAD: RMS of the (post-denoise) frame vs. the configured
 // threshold, squashed to [0,1] via a soft ramp. We intentionally
 // use the post-denoise frame: SpeexDSP's denoise leaves near-silence
 // very close to zero, which gives the VAD a wide dynamic range to
 // distinguish speech from background hiss.
 const int fs = frameSize >0 ? frameSize :0;
 if (fs <=0) {
 lastEnergyVadProb =0.0f;
 return;
 }
 double sumSq =0.0;
 for (int i =0; i < fs; ++i) {
 const int s =static_cast<int>(frame[i]);
 // Clamp to int16 range before squaring: the input should already
 // be in [-32768,32767], but defensively clamp so a stray out-of-
 // range sample (e.g. from a buggy upstream resampler) cannot push
 // the RMS into the billions and pin the VAD at1.0.
 if (s >32767) sumSq += static_cast<double>(32767) *32767;
 else if (s <-32768) sumSq += static_cast<double>(32768) *32768;
 else sumSq += static_cast<double>(s) * static_cast<double>(s);
 }
 const double rms = std::sqrt(sumSq / static_cast<double>(fs));
 const float thr = energyVadThreshold >0.0f ? energyVadThreshold :500.0f;
 const float ratio = static_cast<float>(rms) / thr;
 // Soft ramp: ratio<=0.25 -> ~0, ratio>=4 -> ~1, smooth in between.
 // We use a clamped linear ramp with a small knee so that very quiet
 // frames (background noise floor) map cleanly to0 and clearly
 // voiced frames (a few times the threshold) map cleanly to1,
 // without needing a full sigmoid in this foundation placeholder.
 float prob;
 if (ratio <=0.25f) prob =0.0f;
 else if (ratio >=4.0f) prob =1.0f;
 else prob = (ratio -0.25f) / (4.0f -0.25f);
 if (prob <0.0f) prob =0.0f;
 else if (prob >1.0f) prob =1.0f;
 lastEnergyVadProb = prob;
 }
#else
 SpeexDSPAux::SpeexDSPAux()
 : state(nullptr), sampleRate(0), frameSize(0), configured(false),
 vadEnabled(false), energyVadEnabled(false),
 energyVadThreshold(500.0f), lastEnergyVadProb(0.0f) {}
 SpeexDSPAux::~SpeexDSPAux() {}
 void SpeexDSPAux::configure(int, int) { configured = false; vadEnabled = false; energyVadEnabled = false; lastEnergyVadProb =0.0f; }
 void SpeexDSPAux::reset() { lastEnergyVadProb =0.0f; }
 void SpeexDSPAux::setEnergyVadEnabled(bool enable, float threshold) {
 energyVadEnabled = enable;
 if (threshold >0.0f) energyVadThreshold = threshold;
 }
 float SpeexDSPAux::getSpeechProbability() const { return -1.0f; }
 bool SpeexDSPAux::lastFrameIsSpeech() const { return false; }
 void SpeexDSPAux::runPreprocess(int16_t*) {}
#endif

} // namespace TelephonyDSP
