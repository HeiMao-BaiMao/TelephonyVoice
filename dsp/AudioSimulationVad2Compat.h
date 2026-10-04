#pragma once
#include "typedef.h"
// Current OpenCORE no longer ships l_deposit_h.h although its dormant VAD2
// FFT still includes it. Multiplication expresses the exact signed Q15->Q31
// deposit without undefined signed-left-shift behavior.
static inline Word32 telephony_vad2_deposit_h(Word16 value) {
    return static_cast<Word32>(value) * 65536;
}
#define L_deposit_h telephony_vad2_deposit_h
