#pragma once

#include <cstdint>
#include <vector>

namespace TelephonyDSP {

 class SpeexDSPAux {
 public:
 SpeexDSPAux();
 ~SpeexDSPAux();
 void configure(int sampleRate, int frameSize);
 void reset();
 // Enable / disable the local energy-based VAD. `threshold` is the
 // RMS (over int16 sample magnitudes) at which the VAD reports
 // probability ~0.5; values below that map to <0.5, above to >0.5.
 // Defaults to500.0f (~-36 dBFS RMS for a sine, a sensible floor
 // for voiced speech on a typical handset mic). `configure()` also
 // auto-scales the threshold by sqrt(frameSize/160) so longer
 // frames don't bias the decision downward. No-op when SpeexDSP is
 // not compiled in (the no-experimental stub keeps the flag but
 // ignores it).
 void setEnergyVadEnabled(bool enable, float threshold =500.0f);
 // Returns the cached energy-VAD probability in [0,1] for the last
 // frame processed by runPreprocess(). Returns -1.0f ("unknown")
 // when SpeexDSP is not compiled in, when the helper is not
 // configured, when no frame has been processed yet, or when the
 // energy VAD is disabled (so callers can distinguish "no signal"
 // from "definitely not speech").
 float getSpeechProbability() const;
 // Returns true if the last run() call was classified as speech by
 // the energy VAD (lastEnergyVadProb >0.5). Always returns false
 // when SpeexDSP is not compiled in, when the helper is not
 // configured, or when the energy VAD is disabled.
 bool lastFrameIsSpeech() const;
 void runPreprocess(int16_t* frame); // in-place, no-op when disabled
 private:
 void* state; // SpeexPreprocessState* kept void* to avoid speex headers here
 int sampleRate;
 int frameSize;
 bool configured;
 // Tracks whether SPEEX_PREPROCESS_SET_VAD is enabled on `state`.
 // SpeexDSP's VAD is currently a placeholder that logs
 // "The VAD has been replaced by a hack pending a complete rewrite"
 // every time it is enabled, so we leave it off and expose it as a
 // separate flag rather than a derived state. While this is false,
 // getSpeechProbability() returns -1.0f ("unknown") and
 // lastFrameIsSpeech() returns false, instead of a `0.0f / false`
 // pair that would falsely imply "definitely not speech".
 bool vadEnabled;
 // Local energy-based VAD (replaces the disabled SpeexDSP VAD).
 // When `energyVadEnabled` is true, runPreprocess() computes the
 // frame RMS, divides by `energyVadThreshold`, and squashes the
 // ratio to [0,1] via a soft ramp; the result is cached in
 // `lastEnergyVadProb` and exposed through getSpeechProbability()
 // / lastFrameIsSpeech().
 bool energyVadEnabled;
 float energyVadThreshold;
 float lastEnergyVadProb;
 // Private copy of the frame the SpeexDSP preprocessor runs on.  The
 // denoiser feeds the energy VAD only; the caller's frame is left
 // untouched so the denoiser cannot filter the audio path (see
 // runPreprocess()).
 std::vector<int16_t> vadScratch;
 };

} // namespace TelephonyDSP
