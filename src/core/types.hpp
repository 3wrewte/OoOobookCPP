#ifndef TYPES_H
#define TYPES_H
#include<cstdint>
#include<numeric>

const size_t Price_bits = 28;
const size_t   Pos_bits = 28;
const size_t   Sym_bits = 7;
using Price_t   = uint32_t;   // 28 valid bit
using Pos_t     = uint32_t;   // 28 valid bit
using Sym_t     = uint8_t ;   // 7  valid bit 

const size_t     Qty_bits = 28;
const size_t OrderID_bits = 32;
using Qty_t     = uint32_t;
using OrderID_t = uint32_t;

using OpSeq   = uint32_t;
using MopSeq  = uint32_t;
using UopSeq  = uint64_t;
using Seq     = uint64_t;

const size_t   QtySum_bits = 32;
const size_t QtyDelta_bits = 32;
using QtySum_t     = uint32_t;   // line_sum/level_sum/tail
using QtyDelta_t   = int32_t;    // planned_delta 等有符号

const size_t Side_bits = 1;
enum Side_t : bool {Buy = 0, Sell = 1};


template <size_t Bits>
inline uint64_t saturating_add64(uint64_t a, uint64_t b){
    assert(Bits <= 64);
    uint64_t val = std::saturating_add(a, b);
    uint64_t mask = ~((~uint64_t(0)) << Bits);
    return val & mask;
}

template <size_t Bits>
inline uint32_t saturating_add32(uint32_t a, uint32_t b){
    assert(Bits <= 32);
    uint32_t val = std::saturating_add(a, b);
    uint32_t mask = ~((~uint32_t(0)) << Bits);
    return val & mask;
}

template <size_t Bits>
inline uint8_t saturating_add8(uint8_t a, uint8_t b){
    assert(Bits <= 8);
    uint8_t val = std::saturating_add(a, b);
    uint8_t mask = ~((~uint8_t(0)) << Bits);
    return val & mask;
}

#endif