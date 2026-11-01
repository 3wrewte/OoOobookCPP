#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <unordered_map>
#include <vector>

#include "src/core/addr.hpp"
#include "src/core/sumcache.hpp"
#include "src/core/types.hpp"

// Ports of the Rust `sumcache.rs` tests adapted to the current C++ reference
// stores (`LineSumMap` / `LevelSumMap`). The Rust eviction/refill test is
// intentionally deferred: these temporary stores have no direct-mapped window.
// Rust's saturating-underflow case is replaced by valid decrement-to-zero:
// the C++ stores assert on underflow instead of saturating, so an underflowing
// delta would abort the test process rather than produce 0.

namespace {

constexpr size_t kSlots = 16;
constexpr QtySum_t kMaxSum = (1ULL << QtySum_bits) - 1;

CanonicalAddrStructured addr(Sym_t sym, Pos_t pos, Price_t price, Side_t side) {
    CanonicalAddrStructured a{sym, side, price, pos};
    REQUIRE(CanonicalAddrStructuredValid(a));
    return a;
}

LineSumCacheView line_at(Sym_t sym, Pos_t pos, Price_t price, Side_t side) {
    return LineSumCacheIndexing(addr(sym, pos, price, side));
}

LevelSumCacheView level_at(Sym_t sym, Pos_t pos, Price_t price, Side_t side) {
    return LevelSumCacheIndexing(addr(sym, pos, price, side));
}

LineSumCacheView line_raw(LineSum_tag_t tag, LineSum_index_t index,
                          LineSum_offset_t offset) {
    LineSumCacheView v{tag, index, offset};
    REQUIRE(LineSumCacheViewValid(v));
    return v;
}

LevelSumCacheView level_raw(LevelSum_tag_t tag, LevelSum_index_t index,
                            LevelSum_offset_t offset) {
    LevelSumCacheView v{tag, index, offset};
    REQUIRE(LevelSumCacheViewValid(v));
    return v;
}

std::array<QtySum_t, kSlots> zero_row() {
    return std::array<QtySum_t, kSlots>();
}

std::vector<CanonicalAddrStructured> random_addrs(uint64_t seed, size_t count) {
    std::mt19937_64 rng(seed);
    std::vector<CanonicalAddrStructured> addrs;
    addrs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Sym_t sym = static_cast<Sym_t>(rng() & ((1ULL << Sym_bits) - 1));
        Pos_t pos = static_cast<Pos_t>(rng() & ((1ULL << Pos_bits) - 1));
        Price_t price = static_cast<Price_t>(rng() & ((1ULL << Price_bits) - 1));
        Side_t side = (rng() & 1) ? Side_t::Sell : Side_t::Buy;
        addrs.push_back(CanonicalAddrStructured{sym, side, price, pos});
    }
    return addrs;
}

template <typename Map, typename View, typename IndexFn, typename KeyFn>
void check_property_against_reference(Map& map,
                                      const std::vector<CanonicalAddrStructured>& addrs,
                                      IndexFn index, KeyFn key_of) {
    std::unordered_map<uint64_t, std::array<QtySum_t, kSlots>> reference;
    std::mt19937_64 rng(0x51C0FFEEULL);

    for (size_t step = 0; step < 1000; ++step) {
        const auto& a = addrs[rng() % addrs.size()];
        View v = index(a);
        const uint64_t key = key_of(v);
        const size_t offset = v.offset;
        auto& ref_row = reference[key];

        const QtySum_t current = ref_row[offset];
        const int64_t min_delta = -static_cast<int64_t>(current);
        const int64_t max_delta = static_cast<int64_t>(kMaxSum - current);
        int64_t delta = static_cast<int64_t>(rng() % 2001) - 1000;
        if (delta < min_delta) delta = min_delta;
        if (delta > max_delta) delta = max_delta;

        map.apply_delta(v, delta);
        ref_row[offset] = static_cast<QtySum_t>(static_cast<int64_t>(current) + delta);

        CHECK(map.read_single_through(v) == ref_row[offset]);
        CHECK(map.read_row_through(v) == ref_row);

        size_t nonzero_rows = 0;
        for (const auto& [ref_key, row] : reference) {
            bool nonzero = false;
            for (QtySum_t value : row) {
                if (value != 0) {
                    nonzero = true;
                    break;
                }
            }
            if (nonzero) {
                ++nonzero_rows;
                CHECK(map.ram.contains(ref_key));
            }
        }
        CHECK(map.ram.size() == nonzero_rows);
    }
}

} // namespace

TEST_CASE("line sum delta accumulates and persists") {
    LineSumMap map;
    const LineSumCacheView v = line_at(0, 3, 0x10, Side_t::Sell);

    map.apply_delta(v, 7);
    map.apply_delta(v, 3);

    CHECK(map.read_single_through(v) == 10);
    CHECK(map.ram.contains(get_LineSum_key(v)));

    const auto row = map.read_row_through(v);
    CHECK(row[v.offset] == 10);
    for (size_t i = 0; i < kSlots; ++i) {
        if (i != v.offset) {
            CHECK(row[i] == 0);
        }
    }
}

TEST_CASE("line sum negative delta can return a slot to zero") {
    LineSumMap map;
    const LineSumCacheView v = line_at(0, 3, 0x10, Side_t::Sell);
    const uint64_t key = get_LineSum_key(v);

    map.apply_delta(v, 5);
    map.apply_delta(v, -3);
    CHECK(map.read_single_through(v) == 2);

    map.apply_delta(v, -2);
    CHECK(map.read_single_through(v) == 0);
    CHECK_FALSE(map.ram.contains(key));
}

TEST_CASE("line sum all-zero row is removed from ram") {
    LineSumMap map;
    const LineSumCacheView v = line_at(0, 20, 0x10, Side_t::Buy);
    const uint64_t key = get_LineSum_key(v);

    map.apply_delta(v, 4);
    CHECK(map.ram.size() == 1);

    map.apply_delta(v, -4);
    CHECK(map.read_single_through(v) == 0);
    CHECK_FALSE(map.ram.contains(key));
    CHECK(map.ram.empty());
}

TEST_CASE("line sum slots within one row are independent") {
    LineSumMap map;
    const LineSumCacheView slot0 = line_at(0, 3, 0x10, Side_t::Sell);
    const LineSumCacheView slot1 = line_at(0, 20, 0x10, Side_t::Sell);

    REQUIRE(get_LineSum_key(slot0) == get_LineSum_key(slot1));
    REQUIRE(slot0.offset != slot1.offset);

    map.apply_delta(slot0, 10);
    map.apply_delta(slot1, 20);

    const auto row = map.read_row_through(slot0);
    CHECK(row[slot0.offset] == 10);
    CHECK(row[slot1.offset] == 20);
    for (size_t i = 0; i < kSlots; ++i) {
        if (i != slot0.offset && i != slot1.offset) {
            CHECK(row[i] == 0);
        }
    }
}

TEST_CASE("line sum view groups one id-vol line per slot") {
    LineSumMap map;
    const LineSumCacheView same_line_a = line_at(0, 3, 0x10, Side_t::Sell);
    const LineSumCacheView same_line_b = line_at(0, 9, 0x10, Side_t::Sell);

    CHECK(get_LineSum_key(same_line_a) == get_LineSum_key(same_line_b));
    CHECK(same_line_a.offset == same_line_b.offset);

    map.apply_delta(same_line_a, 5);
    CHECK(map.read_single_through(same_line_b) == 5);

    // pos[7:4] changes: same line key, different slot.
    const LineSumCacheView next_line_slot = line_at(0, 20, 0x10, Side_t::Sell);
    CHECK(get_LineSum_key(next_line_slot) == get_LineSum_key(same_line_a));
    CHECK(next_line_slot.offset != same_line_a.offset);
    CHECK(map.read_single_through(next_line_slot) == 0);

    // pos[27:8] changes: different line key.
    const LineSumCacheView other_line = line_at(0, 256, 0x10, Side_t::Sell);
    CHECK(get_LineSum_key(other_line) != get_LineSum_key(same_line_a));
    CHECK(map.read_single_through(other_line) == 0);
}

TEST_CASE("line sum miss does not allocate a ram row") {
    LineSumMap map;
    const LineSumCacheView v = line_at(0, 7, 0x20, Side_t::Buy);

    CHECK(map.read_single_through(v) == 0);
    CHECK(map.read_row_through(v) == zero_row());
    CHECK(map.ram.empty());

    map.apply_delta(v, 0);
    CHECK(map.ram.empty());
}

TEST_CASE("line sum different side and price are independent") {
    LineSumMap map;
    const LineSumCacheView buy = line_at(0, 3, 0x20, Side_t::Buy);
    const LineSumCacheView sell = line_at(0, 3, 0x20, Side_t::Sell);
    const LineSumCacheView other_price = line_at(0, 3, 0x30, Side_t::Buy);

    map.apply_delta(buy, 10);
    map.apply_delta(sell, 20);
    map.apply_delta(other_price, 30);

    CHECK(map.read_single_through(buy) == 10);
    CHECK(map.read_single_through(sell) == 20);
    CHECK(map.read_single_through(other_price) == 30);
}

TEST_CASE("line sum accepts the maximum representable row value") {
    LineSumMap map;
    const LineSumCacheView v = line_raw(0, 0, 0);
    const QtyDelta_t max_delta = std::numeric_limits<QtyDelta_t>::max();

    REQUIRE(static_cast<QtySum_t>(max_delta) == kMaxSum);

    map.apply_delta(v, max_delta);
    CHECK(map.read_single_through(v) == kMaxSum);

    map.apply_delta(v, -max_delta);
    CHECK(map.read_single_through(v) == 0);
    CHECK(map.ram.empty());
}

TEST_CASE("level sum delta accumulates and persists") {
    LevelSumMap map;
    const LevelSumCacheView v = level_at(0, 0, 0x1234, Side_t::Buy);

    map.apply_delta(v, 7);
    map.apply_delta(v, 3);

    CHECK(map.read_single_through(v) == 10);
    CHECK(map.ram.contains(get_LevelSum_key(v)));

    const auto row = map.read_row_through(v);
    CHECK(row[v.offset] == 10);
    for (size_t i = 0; i < kSlots; ++i) {
        if (i != v.offset) {
            CHECK(row[i] == 0);
        }
    }
}

TEST_CASE("level sum negative delta can return a slot to zero") {
    LevelSumMap map;
    const LevelSumCacheView v = level_at(0, 0, 0x1234, Side_t::Buy);
    const LevelSum_key_t key = get_LevelSum_key(v);

    map.apply_delta(v, 5);
    map.apply_delta(v, -3);
    CHECK(map.read_single_through(v) == 2);

    map.apply_delta(v, -2);
    CHECK(map.read_single_through(v) == 0);
    CHECK_FALSE(map.ram.contains(key));
}

TEST_CASE("level sum all-zero row is removed from ram") {
    LevelSumMap map;
    const LevelSumCacheView v = level_at(0, 0, 0x1234, Side_t::Buy);
    const LevelSum_key_t key = get_LevelSum_key(v);

    map.apply_delta(v, 4);
    CHECK(map.ram.size() == 1);

    map.apply_delta(v, -4);
    CHECK(map.read_single_through(v) == 0);
    CHECK_FALSE(map.ram.contains(key));
    CHECK(map.ram.empty());
}

TEST_CASE("level sum slots within one row are independent") {
    LevelSumMap map;
    const LevelSumCacheView slot4 = level_at(0, 0, 0x1234, Side_t::Buy);
    const LevelSumCacheView slot5 = level_at(0, 0, 0x1235, Side_t::Buy);

    REQUIRE(get_LevelSum_key(slot4) == get_LevelSum_key(slot5));
    REQUIRE(slot4.offset != slot5.offset);

    map.apply_delta(slot4, 10);
    map.apply_delta(slot5, 20);

    const auto row = map.read_row_through(slot4);
    CHECK(row[slot4.offset] == 10);
    CHECK(row[slot5.offset] == 20);
    for (size_t i = 0; i < kSlots; ++i) {
        if (i != slot4.offset && i != slot5.offset) {
            CHECK(row[i] == 0);
        }
    }
}

TEST_CASE("level sum view pins one price per slot") {
    LevelSumMap map;
    const LevelSumCacheView asking_a = level_at(0, 0, 0x1234, Side_t::Buy);
    const LevelSumCacheView asking_b = level_at(0, 9999, 0x1234, Side_t::Buy);

    CHECK(get_LevelSum_key(asking_a) == get_LevelSum_key(asking_b));
    CHECK(asking_a.offset == asking_b.offset);

    map.apply_delta(asking_a, 5);
    CHECK(map.read_single_through(asking_b) == 5);

    // Different price low nibble: same row key, different slot.
    const LevelSumCacheView next_price = level_at(0, 0, 0x1235, Side_t::Buy);
    CHECK(get_LevelSum_key(next_price) == get_LevelSum_key(asking_a));
    CHECK(next_price.offset != asking_a.offset);
    CHECK(map.read_single_through(next_price) == 0);

    // Different side: different index/key.
    const LevelSumCacheView sell = level_at(0, 0, 0x1234, Side_t::Sell);
    CHECK(get_LevelSum_key(sell) != get_LevelSum_key(asking_a));
    CHECK(map.read_single_through(sell) == 0);
}

TEST_CASE("level sum miss does not allocate a ram row") {
    LevelSumMap map;
    const LevelSumCacheView v = level_at(0, 0, 0x2345, Side_t::Sell);

    CHECK(map.read_single_through(v) == 0);
    CHECK(map.read_row_through(v) == zero_row());
    CHECK(map.ram.empty());

    map.apply_delta(v, 0);
    CHECK(map.ram.empty());
}

TEST_CASE("level sum accepts the maximum representable row value") {
    LevelSumMap map;
    const LevelSumCacheView v = level_raw(0, 0, 0);
    const QtyDelta_t max_delta = std::numeric_limits<QtyDelta_t>::max();

    map.apply_delta(v, max_delta);
    CHECK(map.read_single_through(v) == kMaxSum);

    map.apply_delta(v, -max_delta);
    CHECK(map.read_single_through(v) == 0);
    CHECK(map.ram.empty());
}

TEST_CASE("sum cache view validity rejects out-of-range fields") {
    CHECK(LineSumCacheViewValid(line_raw(0, 0, 0)));
    CHECK(LineSumCacheViewValid(
        LineSumCacheView{(1ULL << LineSum_tag_bits) - 1,
                         static_cast<LineSum_index_t>((1u << LineSum_index_bits) - 1),
                         static_cast<LineSum_offset_t>((1u << LineSum_offset_bits) - 1)}));

    CHECK_FALSE(LineSumCacheViewValid(LineSumCacheView{1ULL << LineSum_tag_bits, 0, 0}));
    CHECK_FALSE(LineSumCacheViewValid(LineSumCacheView{0, 32, 0}));
    CHECK_FALSE(LineSumCacheViewValid(LineSumCacheView{0, 0, 16}));

    CHECK(LevelSumCacheViewValid(level_raw(0, 0, 0)));
    CHECK(LevelSumCacheViewValid(
        LevelSumCacheView{(1u << LevelSum_tag_bits) - 1,
                          static_cast<LevelSum_index_t>((1u << LevelSum_index_bits) - 1),
                          static_cast<LevelSum_offset_t>((1u << LevelSum_offset_bits) - 1)}));

    CHECK_FALSE(LevelSumCacheViewValid(LevelSumCacheView{1u << LevelSum_tag_bits, 0, 0}));
    CHECK_FALSE(LevelSumCacheViewValid(LevelSumCacheView{0, 32, 0}));
    CHECK_FALSE(LevelSumCacheViewValid(LevelSumCacheView{0, 0, 16}));
}

TEST_CASE("line sum clear empties ram") {
    LineSumMap map;
    map.apply_delta(line_at(0, 3, 0x10, Side_t::Sell), 5);
    map.apply_delta(line_at(1, 4, 0x20, Side_t::Buy), 7);
    REQUIRE(!map.ram.empty());

    map.clear();
    CHECK(map.ram.empty());
}

TEST_CASE("level sum clear empties ram") {
    LevelSumMap map;
    map.apply_delta(level_at(0, 0, 0x10, Side_t::Sell), 5);
    map.apply_delta(level_at(1, 0, 0x20, Side_t::Buy), 7);
    REQUIRE(!map.ram.empty());

    map.clear();
    CHECK(map.ram.empty());
}

TEST_CASE("line sum map matches a reference model under random valid deltas") {
    LineSumMap map;
    const auto addrs = random_addrs(0x1234ULL, 32);
    check_property_against_reference<LineSumMap, LineSumCacheView>(
        map, addrs,
        [](const CanonicalAddrStructured& a) { return LineSumCacheIndexing(a); },
        [](const LineSumCacheView& v) { return get_LineSum_key(v); });
}

TEST_CASE("level sum map matches a reference model under random valid deltas") {
    LevelSumMap map;
    const auto addrs = random_addrs(0x5678ULL, 32);
    check_property_against_reference<LevelSumMap, LevelSumCacheView>(
        map, addrs,
        [](const CanonicalAddrStructured& a) { return LevelSumCacheIndexing(a); },
        [](const LevelSumCacheView& v) { return get_LevelSum_key(v); });
}
