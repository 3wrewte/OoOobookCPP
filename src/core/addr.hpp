#ifndef ADDR_H
#define ADDR_H
#include<stdint.h>
#include<stddef.h>
#include<tuple>
#include<assert.h>
#include"types.hpp"

constexpr size_t CanonicalAddr_bits   = 64;
using CanonicalAddr_t   = uint64_t; // { sym, pos[27:4], price[27:4], B/S, price[3:0], pos[3:0] } 64 bits
constexpr size_t IDVol_tag_bits       = 55; // { sym, pos[27:4], price[27:4] }  55 bits
constexpr size_t IDVol_index_bits     = 5 ; // { B/S, price[3:0] } 5 bits -> 16 * 2 lines
constexpr size_t IDVol_offset_bits    = 4 ; // {pos[3:0]}  4 bits -> 16 per line    
using IDVol_tag_t       = uint64_t; // { sym, pos[27:4], price[27:4] }  55 bits
using IDVol_index_t     = uint8_t ; // { B/S, price[3:0] } 5 bits -> 16 * 2 lines
using IDVol_offset_t    = uint8_t ; // {pos[3:0]}  4 bits -> 16 per line   

constexpr size_t LineSum_tag_bits     = 51; // { sym, price[27:4], pos[27:8] }    51 bits
constexpr size_t LineSum_index_bits   = 5 ; // { B/S, price[3:0] }                5 bits -> 16 * 2 lines
constexpr size_t LineSum_offset_bits  = 4 ; // {pos[7:4]}                         4 bits -> 16 line-sums per line
using LineSum_tag_t     = uint64_t; // { sym, price[27:4], pos[27:8] }    51 bits
using LineSum_index_t   = uint8_t ; // { B/S, price[3:0] }                5 bits -> 16 * 2 lines
using LineSum_offset_t  = uint8_t ; // {pos[7:4]}                         4 bits -> 16 line-sums per line

constexpr size_t LevelSum_tag_bits    = 27; // { sym, price[27:8] }  27 bits
constexpr size_t LevelSum_index_bits  = 5 ; // { B/S, price[7:4] }   5 bits -> 16 * 2 lines
constexpr size_t LevelSum_offset_bits = 4 ; // {price[3:0] }         4 bits -> 16 level-sums per line
using LevelSum_tag_t    = uint32_t; // { sym, price[27:8] }  27 bits
using LevelSum_index_t  = uint8_t ; // { B/S, price[7:4] }   5 bits -> 16 * 2 lines
using LevelSum_offset_t = uint8_t ; // {price[3:0] }         4 bits -> 16 level-sums per line

constexpr size_t PriceMetaAddr_bits   = 36; // {sym, B/S, price} 36 bits
using PriceMetaAddr_t   = uint64_t; // {sym, B/S, price} 36 bits

CanonicalAddr_t CanonicalAddrPack(const Sym_t &sym, const Side_t &side, const Price_t &price, const Pos_t &pos){
    assert(  sym <= 1 <<   Sym_bits);
    assert(price <= 1 << Price_bits);
    assert(  pos <= 1 <<   Pos_bits);
    return (sym << 57) | ((pos & 0x0ffffff0) << 33) | ((price & 0x0ffffff0) << 9) | ((side & 0b1) << 8) | ((price & 0x0f) << 4) | (pos & 0x0f);
}
std::tuple<Sym_t, Side_t, Price_t, Pos_t> CanonicalAddrUnpack(const CanonicalAddr_t &addr){
    return std::make_tuple(
        Sym_t  ((addr & 0xe000'0000'0000'0000) >> 57),
        Side_t ((addr & 0x0000'0000'0000'0100) >> 8),
        Price_t(((addr & 0x0000'0001'ffff'fe00) >> 5)  | ((addr & 0xf0) >> 4)),
        Pos_t  (((addr & 0x1fff'fffe'0000'0000) >> 29) |  (addr & 0x0f))
        );
}

std::tuple<IDVol_tag_t, IDVol_index_t, IDVol_offset_t> CanonicalCacheIndexing (const CanonicalAddr_t &addr){
    return std::make_tuple(
        IDVol_tag_t   ((addr & 0xffff'ffff'ffff'fe00)),
        IDVol_index_t ((addr & 0x0000'0000'0000'01f0)),
        IDVol_offset_t((addr & 0x0000'0000'0000'000f))
    );
}
CanonicalAddr_t  CanonicalCacheIndexMerge(const IDVol_tag_t &tag, const IDVol_index_t &index, const IDVol_offset_t &offset){
    assert(   tag <= 1 <<    IDVol_tag_bits);
    assert( index <= 1 <<  IDVol_index_bits);
    assert(offset <= 1 << IDVol_offset_bits);
    return (tag << 9) | (index << 4) | offset;
}
#endif