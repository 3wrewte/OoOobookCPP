#ifndef ADDR_H
#define ADDR_H
#include <cstdint>
#include <cstddef>
#include <bit>
#include <tuple>
#include <cassert>
#include <optional>
#include "types.hpp"

// Canonical 64-bit order address:
//   MSB                                                        LSB
//   +-------+-----------+------------+-----+------------+----------+
//   | sym   | pos[27:4] | price[27:4]| B/S | price[3:0] | pos[3:0] |
//   | 7 b   | 24 b      | 24 b       | 1 b | 4 b        | 4 b      |
//   +-------+-----------+------------+-----+------------+----------+

// line-sum cache decomposition.
constexpr size_t CanonicalAddr_bits   = 64;
using CanonicalAddr_t   = uint64_t; // { sym, pos[27:4], price[27:4], B/S, price[3:0], pos[3:0] } 64 bits
constexpr size_t IDVol_tag_bits       = 55; // { sym, pos[27:4], price[27:4] }  55 bits
constexpr size_t IDVol_index_bits     = 5 ; // { B/S, price[3:0] } 5 bits -> 16 * 2 lines
constexpr size_t IDVol_offset_bits    = 4 ; // {pos[3:0]}  4 bits -> 16 per line    
constexpr size_t IDVol_key_bits       = IDVol_tag_bits + IDVol_index_bits;
using IDVol_tag_t       = uint64_t; // { sym, pos[27:4], price[27:4] }  55 bits
using IDVol_index_t     = uint8_t ; // { B/S, price[3:0] } 5 bits -> 16 * 2 lines
using IDVol_offset_t    = uint8_t ; // {pos[3:0]}  4 bits -> 16 per line   
using IDVol_key_t       = uint64_t; // {tag, index}

// level-sum cache decomposition.
constexpr size_t LineSum_tag_bits     = 51; // { sym, price[27:4], pos[27:8] }    51 bits
constexpr size_t LineSum_index_bits   = 5 ; // { B/S, price[3:0] }                5 bits -> 16 * 2 lines
constexpr size_t LineSum_offset_bits  = 4 ; // {pos[7:4]}                         4 bits -> 16 line-sums per line
constexpr size_t LineSum_key_bits     = LineSum_tag_bits + LineSum_index_bits;
using LineSum_tag_t     = uint64_t; // { sym, price[27:4], pos[27:8] }    51 bits
using LineSum_index_t   = uint8_t ; // { B/S, price[3:0] }                5 bits -> 16 * 2 lines
using LineSum_offset_t  = uint8_t ; // {pos[7:4]}                         4 bits -> 16 line-sums per line
using LineSum_key_t     = uint64_t; // {tag, index}

// id-vol cache decomposition (direct-mapped).
constexpr size_t LevelSum_tag_bits    = 27; // { sym, price[27:8] }  27 bits
constexpr size_t LevelSum_index_bits  = 5 ; // { B/S, price[7:4] }   5 bits -> 16 * 2 lines
constexpr size_t LevelSum_offset_bits = 4 ; // {price[3:0] }         4 bits -> 16 level-sums per line
constexpr size_t LevelSum_key_bits     = LevelSum_tag_bits + LevelSum_index_bits;
using LevelSum_tag_t    = uint32_t; // { sym, price[27:8] }  27 bits
using LevelSum_index_t  = uint8_t ; // { B/S, price[7:4] }   5 bits -> 16 * 2 lines
using LevelSum_offset_t = uint8_t ; // {price[3:0] }         4 bits -> 16 level-sums per line
using LevelSum_key_t    = uint32_t; // {tag, index}

constexpr size_t LevelAddr_bits   = 36; // {sym, B/S, price} 36 bits
using LevelAddr_t   = uint64_t; // {sym, B/S, price} 36 bits

struct CanonicalAddrStructured {
    Sym_t   sym;   // 7 bits
    Side_t  side;  // 0 = Buy, 1 = Sell (already the side bit)
    Price_t price; // 28 bits
    Pos_t   pos;   // 28 bits, FIFO position within a price level
};

inline bool CanonicalAddrStructuredValid(const CanonicalAddrStructured& a) {
    return a.sym   < (1u << Sym_bits)
        && a.price < (1ull << Price_bits)
        && a.pos   < (1ull << Pos_bits);
}

inline CanonicalAddr_t CanonicalAddrPack(const CanonicalAddrStructured& a){
    assert(CanonicalAddrStructuredValid(a));
    return (CanonicalAddr_t(a.sym) << 57) 
    | ((CanonicalAddr_t(a.pos) & 0x0ffffff0) << 29) 
    | ((CanonicalAddr_t(a.price) & 0x0ffffff0) << 5) 
    | ((a.side & 0b1) << 8) 
    | ((a.price & 0x0f) << 4) 
    | ((a.pos & 0x0f));
}
inline CanonicalAddrStructured  CanonicalAddrUnpack(const CanonicalAddr_t &addr){
    return CanonicalAddrStructured{
        .sym   = Sym_t   ((addr & 0xfe00'0000'0000'0000) >> 57),
        .side  = Side_t  ((addr & 0x0000'0000'0000'0100) >> 8 ),
        .price = Price_t(((addr & 0x0000'0001'ffff'fe00) >> 5 )  |  ((addr & 0xf0) >> 4)),
        .pos   = Pos_t  (((addr & 0x01ff'fffe'0000'0000) >> 29)  |  ((addr & 0x0f)     ))
    };
}

struct LevelAddrStructured {
    Sym_t   sym;   // 7 bits
    Side_t  side;  // 0 = Buy, 1 = Sell (already the side bit)
    Price_t price; // 28 bits
};

inline bool LevelAddrStructuredValid(const LevelAddrStructured& a) {
    return a.sym   < (1u << Sym_bits)
    && a.price < (1ull << Price_bits);
}

inline LevelAddr_t LevelAddrPack(const LevelAddrStructured& a){
    assert(LevelAddrStructuredValid(a));
    return (LevelAddr_t(a.sym) << 29)
    | ((a.side & 0b1) << 28)
    |   a.price;
}
inline LevelAddrStructured  LevelAddrUnpack(const LevelAddr_t &addr){
    return LevelAddrStructured{
        .sym   = Sym_t   ((addr & 0x0000'000f'e000'0000) >> 29),
        .side  = Side_t  ((addr & 0x0000'0000'1000'0000) >> 28),
        .price = Price_t ((addr & 0x0000'0000'0fff'ffff)      )
    };
}
inline LevelAddr_t Canonical_to_LevelAddr(CanonicalAddr_t addr){
    CanonicalAddrStructured addr_structured = CanonicalAddrUnpack(addr);
    return LevelAddrPack(LevelAddrStructured{
        .sym = addr_structured.sym,
        .side = addr_structured.side,
        .price = addr_structured.price
    });
}
inline CanonicalAddr_t Level_to_CanonicalAddr(LevelAddr_t addr){
    LevelAddrStructured addr_structured = LevelAddrUnpack(addr);
    return CanonicalAddrPack(CanonicalAddrStructured{
        .sym = addr_structured.sym,
        .side = addr_structured.side,
        .price = addr_structured.price,
        .pos = 0
    });
}


struct IDVolCacheView {
    IDVol_tag_t    tag   ;   // 55 bits
    IDVol_index_t  index ;   // 5 bits
    IDVol_offset_t offset;   // 4 bits
};
inline bool IDVolCacheViewValid(const IDVolCacheView& a) {
    return a.tag      < (1ull << IDVol_tag_bits   )
        && a.index    < (1    << IDVol_index_bits )
        && a.offset   < (1    << IDVol_offset_bits);
}
inline IDVol_key_t get_IDVol_key(const IDVolCacheView& a){
    return (a.tag << 5) | a.index;
}

inline IDVolCacheView IDVolCacheIndexing (const CanonicalAddr_t &addr){
    return IDVolCacheView{
        .tag    = IDVol_tag_t   ((addr & 0xffff'ffff'ffff'fe00) >> 9),
        .index  = IDVol_index_t ((addr & 0x0000'0000'0000'01f0) >> 4),
        .offset = IDVol_offset_t((addr & 0x0000'0000'0000'000f)     )
    };
}
inline CanonicalAddr_t  IDVolCacheIndexMerge(const IDVolCacheView& a){
    assert(a.   tag <= 1ull <<    IDVol_tag_bits);
    assert(a. index <= 1u  <<  IDVol_index_bits);
    assert(a.offset <= 1u  << IDVol_offset_bits);
    return (a.tag << 9) | (a.index << 4) | a.offset;
}


struct LineSumCacheView {
    LineSum_tag_t    tag;      // 51 bits
    LineSum_index_t  index;    // 5 bits
    LineSum_offset_t offset;   // 4 bits
};

inline bool LineSumCacheViewValid(const LineSumCacheView& a) {
    return a.tag      < (1ull << LineSum_tag_bits   )
        && a.index    < (1    << LineSum_index_bits )
        && a.offset   < (1    << LineSum_offset_bits);
}

inline LineSum_key_t get_LineSum_key(const LineSumCacheView& a){
    return (a.tag << 5) | a.index;
}

inline LineSum_index_t LineSum_key2index(const LineSum_key_t &a){
    return a & 0x1f;
}

inline LineSum_tag_t LineSum_key2tag(const LineSum_key_t &a){
    return a >> 5;
}

inline LineSumCacheView LineSumCacheIndexing (const CanonicalAddrStructured &a){
    assert(CanonicalAddrStructuredValid(a));
    return LineSumCacheView{
        .tag    = LineSum_tag_t   ((uint64_t(a.sym) << 44) | ((uint64_t(a.price) & 0x0fff'fff0) << 16) | ((a.pos & 0x0fff'ff00) >> 8)),
        .index  = LineSum_index_t ((a.side << 4) | (a.price & 0x0000'000f)),
        .offset = LineSum_offset_t((a.pos & 0x0000'00f0) >> 4)
    };
}

struct LevelSumCacheView {
    LevelSum_tag_t    tag;      // 27 bits
    LevelSum_index_t  index;    // 5 bits
    LevelSum_offset_t offset;   // 4 bits
};

inline bool LevelSumCacheViewValid(const LevelSumCacheView& a) {
    return a.tag      < (1u   << LevelSum_tag_bits   )
        && a.index    < (1    << LevelSum_index_bits )
        && a.offset   < (1    << LevelSum_offset_bits);
}

inline LevelSum_key_t get_LevelSum_key(const LevelSumCacheView& a){
    return (a.tag << 5) | a.index;
}

inline LevelSum_index_t LevelSum_key2index(const LevelSum_key_t &a){
    return a & 0x1f;
}

inline LevelSum_tag_t LevelSum_key2tag(const LevelSum_key_t &a){
    return a >> 5;
}

inline LevelSumCacheView LevelSumCacheIndexing (const CanonicalAddrStructured &a){
    assert(CanonicalAddrStructuredValid(a));
    return LevelSumCacheView{
        .tag    = LevelSum_tag_t   ((a.sym << 20) | ((a.price & 0x0fff'ff00) >> 8)),
        .index  = LevelSum_index_t ((a.side << 4) | ((a.price & 0x0000'00f0) >> 4)),
        .offset = LevelSum_offset_t( a.price & 0x0000'000f)
    };
}

inline uint64_t upper_one_mask(uint64_t s){ // including bit s itself  {1, ..., 1, 1(bit s), 0, ...(s zeros in total), 0}
    if (s == 64) return 0ull;
    return  (~0ull) << s;
}

inline uint64_t lower_one_mask(uint64_t s){ // including bit s itself  {0, ..., 0, 1(bit s), 1, ...(s + 1 ones in total), 1}
    if (s >= 63) return ~(0ull);
    else return  (1ull << (s + 1)) - 1;
}

inline std::optional<uint64_t> LSB(uint64_t word){ // MUST check whether word = 0, if true, return nullopt
    if(!word) return std::nullopt;
    return std::countr_zero(word);
}

inline std::optional<uint64_t> MSB(uint64_t word){ // MUST check whether word = 0, if true, return nullopt
    if(!word) return std::nullopt;
    return 63 - std::countl_zero(word);
}
#endif
