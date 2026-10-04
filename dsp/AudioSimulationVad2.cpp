// Compile this translation unit in opencore-amrnb, with the same arithmetic
// symbol isolation and include paths as the bundled codec. This file is never
// built for distribution-only configurations. The algorithm itself is the
// bundled reference-derived vad2.cpp/r_fft.cpp implementation, with local
// compatibility adapters for obsolete function spellings/missing headers.
#include "basic_op.h"
#include "vad2.h"
#include <cstdint>
#include <cstring>

extern "C" {
Word16 telephony_vad2_abs_s(Word16 value) { return abs_s(value); }
Word16 telephony_vad2_shl(Word16 value, Word16 shift, Flag* overflow) { return shl(value, shift, overflow); }
Word32 telephony_vad2_L_shl(Word32 value, Word16 shift, Flag* overflow) { return L_shl(value, shift, overflow); }
Word32 telephony_vad2_L_shr(Word32 value, Word16 shift, Flag* overflow) { return L_shr(value, shift, overflow); }

void* telephony_vad2_create() {
    vadState2* state = nullptr;
    return vad2_init(&state) == 0 ? state : nullptr;
}
void telephony_vad2_destroy(void* handle) {
    auto* state = static_cast<vadState2*>(handle);
    if (state) vad2_exit(&state);
}
void telephony_vad2_reset(void* handle) {
    if (handle) vad2_reset(static_cast<vadState2*>(handle));
}
int telephony_vad2_process(void* handle, const int16_t* input, int ltpFlag,
                          int* hangover, int* burst) {
    if (!handle || !input) return 0;
    auto* state = static_cast<vadState2*>(handle);
    Word16 frame[FRM_LEN];
    static_assert(sizeof(Word16) == sizeof(int16_t), "VAD2 PCM word size mismatch");
    std::memcpy(frame, input, sizeof(frame));
    Flag overflow = 0;
    state->LTP_flag = ltpFlag != 0;
    const auto decision = vad2(frame, state, &overflow);
    if (hangover) *hangover = state->hangover;
    if (burst) *burst = state->burstcount;
    return decision;
}
}
