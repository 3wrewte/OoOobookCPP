#include "doctest.h"
#include "src/core/addr.hpp"

// Port of `crates/exchange-matcher-core/src/addr.rs` unit tests, against the
// current core API (CanonicalAddrStructured / CanonicalAddrPack / the
// IDVolCacheIndexing & sum-cache indexing views).

static CanonicalAddrStructured A(Sym_t sym, Pos_t pos, Price_t price, Side_t side) {
    CanonicalAddrStructured a{ sym, side, price, pos };
    REQUIRE(CanonicalAddrStructuredValid(a));
    return a;
}

static IDVol_key_t line_key(CanonicalAddr_t raw) {
    return get_IDVol_key(IDVolCacheIndexing(raw));
}

TEST_CASE("pack/unpack roundtrip across field boundaries") {
    CanonicalAddrStructured cases[] = {
        A(0, 0, 0, Side_t::Buy),
        A(0, 0, 0, Side_t::Sell),
        A(0, 15, 15, Side_t::Buy),                 // boundary: only low nibbles set
        A(0, 16, 16, Side_t::Sell),                // boundary: cross into high field
        A(0, 0xDEADBEE, 0x1234567, Side_t::Buy),
        A(127, (1u << 28) - 1, (1u << 28) - 1, Side_t::Sell), // max
    };
    for (const auto& a : cases) {
        CanonicalAddr_t raw = CanonicalAddrPack(a);
        CanonicalAddrStructured b = CanonicalAddrUnpack(raw);
        CHECK(b.sym   == a.sym);
        CHECK(b.side  == a.side);
        CHECK(b.price == a.price);
        CHECK(b.pos   == a.pos);
    }
}

TEST_CASE("buy and sell land in different index halves") {
    IDVol_index_t b = IDVolCacheIndexing(CanonicalAddrPack(A(0, 0, 0b10101, Side_t::Buy))).index;
    IDVol_index_t s = IDVolCacheIndexing(CanonicalAddrPack(A(0, 0, 0b10101, Side_t::Sell))).index;
    CHECK(b < 16);
    CHECK(s >= 16);
    CHECK(s - 16 == b);
}

TEST_CASE("index only depends on side and price low nibble") {
    IDVol_index_t a = IDVolCacheIndexing(CanonicalAddrPack(A(3, 0xABCDEF, 0x99910, Side_t::Sell))).index;
    IDVol_index_t b = IDVolCacheIndexing(CanonicalAddrPack(A(0, 0, 0x00010, Side_t::Sell))).index;
    CHECK(a == b);
}

TEST_CASE("offset is pos low nibble") {
    CHECK(IDVolCacheIndexing(CanonicalAddrPack(A(0, 7, 100, Side_t::Buy))).offset == 7);
    CHECK(IDVolCacheIndexing(CanonicalAddrPack(A(0, 31, 100, Side_t::Buy))).offset == 15);
}

TEST_CASE("tag captures sym/pos-high/price-high") {
    IDVol_tag_t t0 = IDVolCacheIndexing(CanonicalAddrPack(A(0, 0, 0x10, Side_t::Buy))).tag;
    IDVol_tag_t t1 = IDVolCacheIndexing(CanonicalAddrPack(A(0, 16, 0x10, Side_t::Buy))).tag;
    CHECK(t0 != t1);
}

TEST_CASE("line_key strips offset only") {
    uint64_t base       = line_key(CanonicalAddrPack(A(0, 0, 0x10, Side_t::Buy)));
    uint64_t same_line  = line_key(CanonicalAddrPack(A(0, 9, 0x10, Side_t::Buy))); // offset differs
    uint64_t other_line = line_key(CanonicalAddrPack(A(0, 16, 0x10, Side_t::Buy))); // pos[27:4] differs
    CHECK(base == same_line);
    CHECK(base != other_line);
}

TEST_CASE("line_sum_view groups one id-vol line per slot") {
    LineSumCacheView a = LineSumCacheIndexing(A(0, 3, 0x10, Side_t::Sell));
    LineSumCacheView b = LineSumCacheIndexing(A(0, 9, 0x10, Side_t::Sell));
    CHECK(get_LineSum_key(a) == get_LineSum_key(b));
    CHECK(a.offset == b.offset);

    LineSumCacheView c = LineSumCacheIndexing(A(0, 20, 0x10, Side_t::Sell)); // pos[7:4] changes
    CHECK(a.offset != c.offset);

    LineSumCacheView d = LineSumCacheIndexing(A(0, 256, 0x10, Side_t::Sell)); // pos[27:8] changes
    CHECK(get_LineSum_key(a) != get_LineSum_key(d));
}

TEST_CASE("level_sum_view pins one price per slot") {
    LevelSumCacheView p = LevelSumCacheIndexing(A(0, 0, 0x1234, Side_t::Buy));
    LevelSumCacheView q = LevelSumCacheIndexing(A(0, 9999, 0x1234, Side_t::Buy));
    CHECK(get_LevelSum_key(p) == get_LevelSum_key(q));
    CHECK(p.offset == q.offset);

    LevelSumCacheView r = LevelSumCacheIndexing(A(0, 0, 0x1235, Side_t::Buy)); // price low nibble differs
    CHECK(p.offset != r.offset);
    CHECK(get_LevelSum_key(p) == get_LevelSum_key(r));

    LevelSumCacheView s = LevelSumCacheIndexing(A(0, 0, 0x1234, Side_t::Sell)); // side differs
    CHECK(p.index != s.index);
}

TEST_CASE("idvol tag/index/offset merge roundtrip") {
    CanonicalAddrStructured a = A(5, 0xABCDEF, 0x123456, Side_t::Sell);
    CanonicalAddr_t raw = CanonicalAddrPack(a);
    CanonicalAddr_t merged = IDVolCacheIndexMerge(IDVolCacheIndexing(raw));
    CHECK(merged == raw);
}

TEST_CASE("from_line_key_offset reconstructs the address") {
    CanonicalAddrStructured a = A(0, 9, 0x10, Side_t::Sell);
    IDVol_key_t key = get_IDVol_key(IDVolCacheIndexing(CanonicalAddrPack(a)));
    CanonicalAddrStructured b = CanonicalAddrUnpack((key << 4) | 9);
    CHECK(b.sym == a.sym);
    CHECK(b.side == a.side);
    CHECK(b.price == a.price);
    CHECK(b.pos == a.pos);
}
