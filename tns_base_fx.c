/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS reference.
 *
 *  Mirrors the float TNS accessor family declared in
 *      external/3gpp-evs/lib_com/prot_fx.h (lines 9747-9778)
 *  and implemented in
 *      external/3gpp-evs/lib_com/tns_base.c (lines 246-419).
 *
 *  Why this parent-side helper exists
 *  ----------------------------------
 *  The upstream float implementation uses the float Decoder_State and
 *  int-based signatures that are incompatible with the fixed-point
 *  build.  The FX bitstream tables in
 *      external/3gpp-evs/lib_com/rom_com_fx.c (lines 18296-18405)
 *  already reference these symbols (ParamsBitMap callbacks), but the
 *  vendored snapshot does not ship a fixed-point implementation of the
 *  accessor family.  Because external/3gpp-evs/ is read-only in this
 *  repo, the FX port lives here and is attached to evs-lib-com-fx via
 *  EVS_FX_EXTRAS_LIB_COM.
 *
 *  Porting rules applied
 *  ---------------------
 *    - int                       -> Word16
 *    - Decoder_State *st         -> Decoder_State_fx *st
 *    - get_next_indice(st, n)    -> get_next_indice_fx(st, n)
 *    - float rom_com.h tables    -> rom_com_fx.h tables (same names)
 *  Float-only TNS filtering code (FIRLattice, IIRLattice, TnsFilter,
 *  ITF_*, etc.) is intentionally NOT ported.
 * ============================================================================ */

#include "options.h"
#include "cnst_fx.h"
#include "prot_fx.h"
#include "rom_com_fx.h"
#include "stl.h"
#include <assert.h>

/* Helper functions for Huffman table coding.  Direct FX port of the
 * static helpers at the top of external/3gpp-evs/lib_com/tns_base.c. */

static Word16 GetBitsFromTable(Word16 value, const Coding codes[], Word16 nSize)
{
    (void)nSize;
    assert((value >= 0) && (value < nSize) && (nSize >= 0) && (nSize <= 256));
    return (Word16)codes[value].nBits;
}

static Word16 EncodeUsingTable(Word16 value, const Coding codes[], Word16 nSize)
{
    (void)nSize;
    assert((value >= 0) && (value < nSize) && (nSize >= 0) && (nSize <= 256));
    return (Word16)codes[value].code;
}

static Word16 DecodeUsingTable(Decoder_State_fx *st, Word16 *pValue, const Coding codes[], Word16 nSize)
{
    UWord16 code = 0;
    UWord16 nBits = 0;
    Word16 valueIndex = nSize;

    assert((nSize >= 0) && (nSize <= 256));

    while (valueIndex == nSize)
    {
        code = (UWord16)((code << 1) + get_next_indice_fx(st, 1));
        ++nBits;
        if (nBits > (UWord16)nSize || nBits > 16)
        {
            st->BER_detect = 1;
            *pValue = 0;
            return -1;
        }
        for (valueIndex = 0; valueIndex < nSize; valueIndex++)
        {
            if ((UWord16)codes[valueIndex].nBits == nBits)
            {
                if ((UWord16)codes[valueIndex].code == code)
                {
                    break;
                }
            }
        }
    }

    if (valueIndex < nSize)
    {
        *pValue = (Word16)codes[valueIndex].value;
    }
    else
    {
        st->BER_detect = 1;
        *pValue = 0;
        return -1;
    }

    return (Word16)nBits;
}

/* Get/Set accessors */

void const *GetTnsFilterOrder(void const *p, Word16 index, Word16 *pValue)
{
    *pValue = (Word16)(((STnsFilter const *)p)[index].order);
    return ((STnsFilter const *)p)[index].coefIndex;
}

void *SetTnsFilterOrder(void *p, Word16 index, Word16 value)
{
    ((STnsFilter *)p)[index].order = (int)value;
    return ((STnsFilter *)p)[index].coefIndex;
}

void const *GetNumOfTnsFilters(void const *p, Word16 index, Word16 *pValue)
{
    *pValue = (Word16)(((STnsData const *)p)[index].nFilters);
    return ((STnsData const *)p)[index].filter;
}

void *SetNumOfTnsFilters(void *p, Word16 index, Word16 value)
{
    ((STnsData *)p)[index].nFilters = (int)value;
    return ((STnsData *)p)[index].filter;
}

void const *GetTnsEnabled(void const *p, Word16 index, Word16 *pValue)
{
    *pValue = (((STnsData const *)p)[index].nFilters > 0) ? 1 : 0;
    return NULL;
}

void *SetTnsEnabled(void *p, Word16 index, Word16 value)
{
    (void)p;
    (void)index;
    (void)value;
    return NULL;
}

void const *GetTnsEnabledSingleFilter(void const *p, Word16 index, Word16 *pValue)
{
    *pValue = (((STnsData const *)p)[index].nFilters > 0) ? 1 : 0;
    return ((STnsData const *)p)[index].filter;
}

void *SetTnsEnabledSingleFilter(void *p, Word16 index, Word16 value)
{
    ((STnsData *)p)[index].nFilters = (int)value;
    return ((STnsData *)p)[index].filter;
}

void const *GetTnsFilterCoeff(void const *p, Word16 index, Word16 *pValue)
{
    *pValue = (Word16)(((int const *)p)[index] + INDEX_SHIFT);
    return NULL;
}

void *SetTnsFilterCoeff(void *p, Word16 index, Word16 value)
{
    ((int *)p)[index] = (int)value - INDEX_SHIFT;
    return NULL;
}

/* SWB TCX20 coefficients */

Word16 GetSWBTCX20TnsFilterCoeffBits(Word16 value, Word16 index)
{
    assert((index >= 0) && (index < nTnsCoeffTables));
    return GetBitsFromTable(value, codesTnsCoeffSWBTCX20[index], nTnsCoeffCodes);
}

Word16 EncodeSWBTCX20TnsFilterCoeff(Word16 value, Word16 index)
{
    assert((index >= 0) && (index < nTnsCoeffTables));
    return EncodeUsingTable(value, codesTnsCoeffSWBTCX20[index], nTnsCoeffCodes);
}

Word16 DecodeSWBTCX20TnsFilterCoeff(Decoder_State_fx *st, Word16 index, Word16 *pValue)
{
    assert((index >= 0) && (index < nTnsCoeffTables));
    return DecodeUsingTable(st, pValue, codesTnsCoeffSWBTCX20[index], nTnsCoeffCodes);
}

/* SWB TCX10 coefficients */

Word16 GetSWBTCX10TnsFilterCoeffBits(Word16 value, Word16 index)
{
    assert((index >= 0) && (index < nTnsCoeffTables));
    return GetBitsFromTable(value, codesTnsCoeffSWBTCX10[index], nTnsCoeffCodes);
}

Word16 EncodeSWBTCX10TnsFilterCoeff(Word16 value, Word16 index)
{
    assert((index >= 0) && (index < nTnsCoeffTables));
    return EncodeUsingTable(value, codesTnsCoeffSWBTCX10[index], nTnsCoeffCodes);
}

Word16 DecodeSWBTCX10TnsFilterCoeff(Decoder_State_fx *st, Word16 index, Word16 *pValue)
{
    assert((index >= 0) && (index < nTnsCoeffTables));
    return DecodeUsingTable(st, pValue, codesTnsCoeffSWBTCX10[index], nTnsCoeffCodes);
}

/* WB TCX20 coefficients */

Word16 GetWBTCX20TnsFilterCoeffBits(Word16 value, Word16 index)
{
    assert((index >= 0) && (index < nTnsCoeffTables));
    return GetBitsFromTable(value, codesTnsCoeffWBTCX20[index], nTnsCoeffCodes);
}

Word16 EncodeWBTCX20TnsFilterCoeff(Word16 value, Word16 index)
{
    assert((index >= 0) && (index < nTnsCoeffTables));
    return EncodeUsingTable(value, codesTnsCoeffWBTCX20[index], nTnsCoeffCodes);
}

Word16 DecodeWBTCX20TnsFilterCoeff(Decoder_State_fx *st, Word16 index, Word16 *pValue)
{
    assert((index >= 0) && (index < nTnsCoeffTables));
    return DecodeUsingTable(st, pValue, codesTnsCoeffWBTCX20[index], nTnsCoeffCodes);
}

/* Filter order */

Word16 GetTnsFilterOrderBitsSWBTCX20(Word16 value, Word16 index)
{
    (void)index;
    return GetBitsFromTable((Word16)(value - 1), codesTnsOrderTCX20, nTnsOrderCodes);
}

Word16 EncodeTnsFilterOrderSWBTCX20(Word16 value, Word16 index)
{
    (void)index;
    return EncodeUsingTable((Word16)(value - 1), codesTnsOrderTCX20, nTnsOrderCodes);
}

Word16 DecodeTnsFilterOrderSWBTCX20(Decoder_State_fx *st, Word16 index, Word16 *pValue)
{
    (void)index;
    return DecodeUsingTable(st, pValue, codesTnsOrderTCX20, nTnsOrderCodes);
}

Word16 GetTnsFilterOrderBitsSWBTCX10(Word16 value, Word16 index)
{
    (void)index;
    return GetBitsFromTable((Word16)(value - 1), codesTnsOrderTCX10, nTnsOrderCodes);
}

Word16 EncodeTnsFilterOrderSWBTCX10(Word16 value, Word16 index)
{
    (void)index;
    return EncodeUsingTable((Word16)(value - 1), codesTnsOrderTCX10, nTnsOrderCodes);
}

Word16 DecodeTnsFilterOrderSWBTCX10(Decoder_State_fx *st, Word16 index, Word16 *pValue)
{
    (void)index;
    return DecodeUsingTable(st, pValue, codesTnsOrderTCX10, nTnsOrderCodes);
}

Word16 GetTnsFilterOrderBits(Word16 value, Word16 index)
{
    (void)index;
    return GetBitsFromTable((Word16)(value - 1), codesTnsOrder, nTnsOrderCodes);
}

Word16 EncodeTnsFilterOrder(Word16 value, Word16 index)
{
    (void)index;
    return EncodeUsingTable((Word16)(value - 1), codesTnsOrder, nTnsOrderCodes);
}

Word16 DecodeTnsFilterOrder(Decoder_State_fx *st, Word16 index, Word16 *pValue)
{
    (void)index;
    return DecodeUsingTable(st, pValue, codesTnsOrder, nTnsOrderCodes);
}
