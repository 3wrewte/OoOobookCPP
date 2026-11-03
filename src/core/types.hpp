#ifndef TYPES_H
#define TYPES_H
#include<cstdint>
#include<cstddef>
#include<numeric>

constexpr size_t Price_bits = 28;
constexpr size_t   Pos_bits = 28;
constexpr size_t   Sym_bits = 7;
using Price_t   = uint32_t;   // 28 valid bit
using Pos_t     = uint32_t;   // 28 valid bit
using Sym_t     = uint8_t ;   // 7  valid bit 
constexpr Price_t Price_max = (1u << Price_bits) - 1;
constexpr Price_t Price_min = 0;

constexpr size_t     Qty_bits = 32; //28 bit if we want QtySum to be 32bit
constexpr size_t OrderID_bits = 32;
using Qty_t     = uint32_t;
using OrderID_t = uint32_t;
constexpr Qty_t Qty_max = (1ull << Qty_bits) - 1;
constexpr Qty_t Qty_min = 0;

using OpSeq   = uint32_t;
using MopSeq  = uint32_t;
using UopSeq  = uint64_t;
using Seq     = uint64_t;

constexpr size_t   QtySum_bits = 63;
constexpr size_t QtyDelta_bits = 64;
using QtySum_t     = uint64_t;   // line_sum/level_sum/tail
using QtyDelta_t   = int64_t;    // planned_delta 等有符号

constexpr size_t Side_bits = 1;
enum Side_t : bool {Buy = 0, Sell = 1};



#endif
