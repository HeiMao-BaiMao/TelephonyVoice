// Compile in opencore-amrnb, so its existing arithmetic ABI isolation applies.
// OpenCORE's dormant VAD2 source still calls the historical `add` spelling;
// the current bundled basic operation is the equivalent saturated add_16.
// Keep this compatibility fix local rather than modifying third-party code.
#define add add_16
#define abs_s telephony_vad2_abs_s
#define shl telephony_vad2_shl
#define L_shl telephony_vad2_L_shl
#define L_shr telephony_vad2_L_shr
#define r_fft telephony_amrnb_vad2_r_fft
#include "../external/opencore-amr/opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/vad2.cpp"
