#include "dsp/Transport.h"
#include "external/opencore-amr/amrnb/interf_enc.h"
#include "external/opencore-amr/amrnb/interf_dec.h"
#include "external/vo-amrwbenc/enc_if.h"
#include "external/opencore-amr/amrwb/dec_if.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

using namespace TelephonyDSP::Transport;
int main() {
    int failures=0,frames=0,sid=0,noData=0;
    for(bool wide:{false,true}) for(int mode=0;mode<(wide?9:8);++mode) {
        void* encoder=wide?E_IF_init():Encoder_Interface_init(1);
        void* decoderA=wide?D_IF_init():Decoder_Interface_init();
        void* decoderB=wide?D_IF_init():Decoder_Interface_init();
        if(!encoder||!decoderA||!decoderB) return 2;
        const auto kind=wide?AmrCodec::Wideband:AmrCodec::Narrowband;
        for(int index=0;index<200;++index) {
            std::array<int16_t,320> input{},a{},b{};std::array<uint8_t,64> encoded{};
            const int samples=wide?320:160;
            if(index<30)for(int i=0;i<samples;++i)input[i]=int16_t(10000*std::sin((i+index*samples)*0.071));
            const int count=wide?E_IF_encode(encoder,mode,input.data(),encoded.data(),1):
                Encoder_Interface_Encode(encoder,static_cast<Mode>(mode),input.data(),encoded.data(),0);
            if(count<=0||count>64){++failures;continue;}
            Bytes storage(encoded.begin(),encoded.begin()+count),wire,recovered;AmrFrame frame;AmrPayload parsed,payload;
            if(!amrFromStorage(storage,kind,frame)){++failures;continue;}
            ++frames;sid+=frame.frameType==(wide?9:8);noData+=frame.frameType==15;
            payload.frames={frame};
            const auto alignment=index%2?AmrAlignment::BandwidthEfficient:AmrAlignment::OctetAligned;
            if(!serializeAmr(payload,kind,alignment,wire)||!parseAmr(wire,kind,alignment,parsed)||
                !amrToStorage(parsed.frames[0],kind,recovered)||recovered!=storage){++failures;continue;}
            if(wide){D_IF_decode(decoderA,storage.data(),a.data(),0);D_IF_decode(decoderB,recovered.data(),b.data(),0);}
            else{Decoder_Interface_Decode(decoderA,storage.data(),a.data(),0);Decoder_Interface_Decode(decoderB,recovered.data(),b.data(),0);}
            if(!std::equal(a.begin(),a.begin()+samples,b.begin()))++failures;
        }
        if(wide){E_IF_exit(encoder);D_IF_exit(decoderA);D_IF_exit(decoderB);}
        else{Encoder_Interface_exit(encoder);Decoder_Interface_exit(decoderA);Decoder_Interface_exit(decoderB);}
    }
    std::printf("AMR transport real codecs: %d frames, %d SID, %d NO_DATA, %d failures\n",frames,sid,noData,failures);
    return failures||frames!=3400||sid==0||noData==0?1:0;
}
