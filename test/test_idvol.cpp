#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <utility>
#include <vector>

#include "src/core/addr.hpp"
#include "src/core/idvol.hpp"
#include "src/core/types.hpp"

// Ports of the Rust `idvol.rs` store/sum tests, adapted to the C++ `IdVolMap`
// reference store. The direct-mapped cache window is intentionally absent from
// the golden model, so the Rust "write-through survives cache eviction" test is
// replaced by sparse-RAM assertions. Inserts follow the pipeline contract
// (`pos == current tail` for the level), so tests insert sequentially; holes are
// created by cancelling a slot instead of skipping a position. `take_line` is the
// apply step: the caller (the uOP at issue) supplies the already-computed delta
// vector and its scalar sum.

namespace {

using VolLine = std::array<Qty_t, 16>;

CanonicalAddr_t A(Sym_t sym, Pos_t pos, Price_t price, Side_t side) {
    CanonicalAddrStructured a{sym, side, price, pos};
    REQUIRE(CanonicalAddrStructuredValid(a));
    return CanonicalAddrPack(a);
}

struct Delta {
    VolLine vec{};
    QtySum_t sum = 0;
};

Delta delta(std::initializer_list<std::pair<size_t, Qty_t>> items) {
    Delta d;
    for (const auto& [off, vol] : items) {
        REQUIRE(off < 16);
        d.vec[off] = vol;
        d.sum += vol;
    }
    return d;
}

QtySum_t line_total(const Line& line) {
    QtySum_t total = 0;
    for (Qty_t v : line.Vols) {
        total += v;
    }
    return total;
}

VolLine full_line(Qty_t per_slot) {
    VolLine dv{};
    dv.fill(per_slot);
    return dv;
}

} // namespace

TEST_CASE("insert then read back through ram") {
    IdVolMap c;
    c.reset();
    const CanonicalAddr_t a = A(0, 0, 100, Side_t::Sell);

    c.insert_slot(a, 11, 7);

    CHECK(c.slot_volume(a) == 7);
    const Line line = c.read_line(a);
    CHECK(line.Vols[0] == 7);
    CHECK(line.IDs[0] == 11);
    CHECK(line_total(line) == 7);
    CHECK(c.line_sum_ram(a) == 7);
    CHECK(c.level_sum_ram(a) == 7);
    CHECK(c.tail_ram(a) == 1);
    CHECK(c.ram.size() == 1);
}

TEST_CASE("take consumes fifo from offset zero") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x10;
    c.insert_slot(A(0, 0, p, Side_t::Sell), 1, 5);
    c.insert_slot(A(0, 1, p, Side_t::Sell), 2, 3);
    c.insert_slot(A(0, 2, p, Side_t::Sell), 3, 4);

    const Delta d = delta({{0, 5}, {1, 3}, {2, 1}}); // 9 consumed
    c.take_line(A(0, 0, p, Side_t::Sell), d.vec, d.sum);

    CHECK(c.slot_volume(A(0, 0, p, Side_t::Sell)) == 0);
    CHECK(c.slot_volume(A(0, 1, p, Side_t::Sell)) == 0);
    CHECK(c.slot_volume(A(0, 2, p, Side_t::Sell)) == 3);
    CHECK(c.line_sum_ram(A(0, 0, p, Side_t::Sell)) == 3);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Sell)) == 3);
}

TEST_CASE("take skips holes without compacting") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x10;
    c.insert_slot(A(0, 0, p, Side_t::Sell), 1, 5);
    c.insert_slot(A(0, 1, p, Side_t::Sell), 2, 2);
    c.insert_slot(A(0, 2, p, Side_t::Sell), 3, 4);

    const CancelResult hole = c.cancel_slot(A(0, 1, p, Side_t::Sell), 2);
    REQUIRE(hole.found);
    CHECK(hole.volume_removed == 2);
    CHECK(c.slot_volume(A(0, 1, p, Side_t::Sell)) == 0);

    const Delta d = delta({{0, 5}, {2, 4}}); // offset 1 is a hole
    c.take_line(A(0, 0, p, Side_t::Sell), d.vec, d.sum);

    CHECK(c.slot_volume(A(0, 0, p, Side_t::Sell)) == 0);
    CHECK(c.slot_volume(A(0, 2, p, Side_t::Sell)) == 0);
    CHECK(c.line_sum_ram(A(0, 0, p, Side_t::Sell)) == 0);
    CHECK(c.ram.empty());
}

TEST_CASE("partial take keeps the untouched remainder") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x20;
    c.insert_slot(A(0, 0, p, Side_t::Buy), 1, 5);

    const Delta d = delta({{0, 3}});
    c.take_line(A(0, 0, p, Side_t::Buy), d.vec, d.sum);

    CHECK(c.slot_volume(A(0, 0, p, Side_t::Buy)) == 2);
    CHECK(c.line_sum_ram(A(0, 0, p, Side_t::Buy)) == 2);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Buy)) == 2);
}

TEST_CASE("cancel clears slot and reports removed volume") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x10;
    c.insert_slot(A(0, 0, p, Side_t::Buy), 9, 8);

    const CancelResult r = c.cancel_slot(A(0, 0, p, Side_t::Buy), 9);
    CHECK(r.found);
    CHECK(r.order_id == 9);
    CHECK(r.volume_removed == 8);
    CHECK(c.slot_volume(A(0, 0, p, Side_t::Buy)) == 0);
    CHECK(c.ram.empty());
    CHECK(c.line_sum_ram(A(0, 0, p, Side_t::Buy)) == 0);

    // The line was removed from RAM, so a second cancel finds nothing.
    const CancelResult r2 = c.cancel_slot(A(0, 1, p, Side_t::Buy), 10);
    CHECK_FALSE(r2.found);
}

TEST_CASE("cancel with volume removes only the requested amount") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x10;
    c.insert_slot(A(0, 0, p, Side_t::Sell), 1, 1);
    c.insert_slot(A(0, 1, p, Side_t::Sell), 2, 1);
    c.insert_slot(A(0, 2, p, Side_t::Sell), 3, 1);
    c.insert_slot(A(0, 3, p, Side_t::Sell), 7, 10);

    const CancelResult r = c.cancel_slot_with_volume(A(0, 3, p, Side_t::Sell), 7, 4);
    CHECK(r.found);
    CHECK(r.order_id == 7);
    CHECK(r.volume_removed == 4);
    CHECK(c.slot_volume(A(0, 3, p, Side_t::Sell)) == 6);
    CHECK(c.line_sum_ram(A(0, 0, p, Side_t::Sell)) == 9);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Sell)) == 9);
}

TEST_CASE("cancel rejects a mismatched order id") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x10;
    c.insert_slot(A(0, 0, p, Side_t::Buy), 7, 5);

    const CancelResult r = c.cancel_slot(A(0, 0, p, Side_t::Buy), 8);
    CHECK_FALSE(r.found);
    CHECK(c.slot_volume(A(0, 0, p, Side_t::Buy)) == 5);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Buy)) == 5);
}

TEST_CASE("buy and sell are isolated") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x10;
    c.insert_slot(A(0, 0, p, Side_t::Buy), 1, 10);
    c.insert_slot(A(0, 0, p, Side_t::Sell), 2, 20);

    const Delta buy = delta({{0, 10}});
    const Delta sell = delta({{0, 20}});
    c.take_line(A(0, 0, p, Side_t::Buy), buy.vec, buy.sum);
    c.take_line(A(0, 0, p, Side_t::Sell), sell.vec, sell.sum);

    CHECK(c.slot_volume(A(0, 0, p, Side_t::Buy)) == 0);
    CHECK(c.slot_volume(A(0, 0, p, Side_t::Sell)) == 0);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Buy)) == 0);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Sell)) == 0);
    CHECK(c.ram.empty());
}

TEST_CASE("ram stays sparse after a full drain") {
    IdVolMap c;
    c.reset();
    const CanonicalAddr_t a = A(0, 0, 100, Side_t::Sell);
    c.insert_slot(a, 1, 5);
    CHECK(c.ram.size() == 1);

    const Delta d = delta({{0, 5}});
    c.take_line(a, d.vec, d.sum);

    CHECK(c.ram.empty());
    CHECK(c.line_sum.ram.empty());
    CHECK(c.level_sum.ram.empty());
}

TEST_CASE("level sum matches inserted volumes") {
    IdVolMap c;
    c.reset();
    c.insert_slot(A(0, 0, 0x120, Side_t::Sell), 1, 5);
    c.insert_slot(A(0, 1, 0x120, Side_t::Sell), 2, 7);
    c.insert_slot(A(0, 0, 0x340, Side_t::Sell), 3, 11);

    CHECK(c.level_sum_ram(A(0, 0, 0x120, Side_t::Sell)) == 12);
    CHECK(c.level_sum_ram(A(0, 0, 0x340, Side_t::Sell)) == 11);
}

TEST_CASE("line sum matches id-vol line total across two lines") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x120;
    // 16 orders fill the first id-vol line (pos 0..15), then 4 spill into the
    // second line (pos 16..19).
    for (Pos_t pos = 0; pos < 16; ++pos) {
        c.insert_slot(A(0, pos, p, Side_t::Sell), pos + 1, 2);
    }
    for (Pos_t pos = 16; pos < 20; ++pos) {
        c.insert_slot(A(0, pos, p, Side_t::Sell), pos + 1, 3);
    }

    CHECK(c.line_sum_ram(A(0, 3, p, Side_t::Sell)) == 16 * 2);
    CHECK(c.line_sum_ram(A(0, 17, p, Side_t::Sell)) == 4 * 3);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Sell)) == 16 * 2 + 4 * 3);
}

TEST_CASE("take decreases both sums by the consumed quantity") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x10;
    c.insert_slot(A(0, 0, p, Side_t::Sell), 1, 10);
    c.insert_slot(A(0, 1, p, Side_t::Sell), 2, 10);

    const Delta d = delta({{0, 7}});
    c.take_line(A(0, 0, p, Side_t::Sell), d.vec, d.sum);

    CHECK(c.slot_volume(A(0, 0, p, Side_t::Sell)) == 3);
    CHECK(c.line_sum_ram(A(0, 0, p, Side_t::Sell)) == 13);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Sell)) == 13);
}

TEST_CASE("cancel decreases both sums") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x10;
    c.insert_slot(A(0, 0, p, Side_t::Buy), 1, 9);
    c.insert_slot(A(0, 1, p, Side_t::Buy), 2, 5);

    const CancelResult r = c.cancel_slot(A(0, 0, p, Side_t::Buy), 1);
    CHECK(r.volume_removed == 9);
    CHECK(c.line_sum_ram(A(0, 0, p, Side_t::Buy)) == 5);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Buy)) == 5);
}

TEST_CASE("line sum row entry is removed after a full take") {
    IdVolMap c;
    c.reset();
    const CanonicalAddr_t a = A(0, 0, 0x10, Side_t::Sell);
    c.insert_slot(a, 1, 6);

    const Delta d = delta({{0, 6}});
    c.take_line(a, d.vec, d.sum);

    CHECK(c.line_sum_ram(a) == 0);
    CHECK(c.line_sum.ram.empty());
    CHECK(c.ram.empty());
}

TEST_CASE("level sum matches ground truth after take and cancel") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x10;
    // Fill line 0 (pos 0..15) with 1 each, then one order in line 1 (pos 16).
    for (Pos_t pos = 0; pos < 16; ++pos) {
        c.insert_slot(A(0, pos, p, Side_t::Sell), pos + 1, 1);
    }
    c.insert_slot(A(0, 16, p, Side_t::Sell), 100, 4);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Sell)) == 20);
    CHECK(c.line_sum_ram(A(0, 0, p, Side_t::Sell)) == 16);
    CHECK(c.line_sum_ram(A(0, 16, p, Side_t::Sell)) == 4);

    const VolLine consume_all = full_line(1);
    c.take_line(A(0, 0, p, Side_t::Sell), consume_all, 16);
    CHECK(c.line_sum_ram(A(0, 0, p, Side_t::Sell)) == 0);
    CHECK(c.line_sum_ram(A(0, 16, p, Side_t::Sell)) == 4);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Sell)) == 4);

    const CancelResult cr = c.cancel_slot(A(0, 16, p, Side_t::Sell), 100);
    CHECK(cr.found);
    CHECK(cr.volume_removed == 4);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Sell)) == 0);
    CHECK(c.ram.empty());
}

TEST_CASE("tail advances per insert and ignores take and cancel") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x10;
    c.insert_slot(A(0, 0, p, Side_t::Buy), 1, 4);
    c.insert_slot(A(0, 1, p, Side_t::Buy), 2, 6);
    CHECK(c.tail_ram(A(0, 0, p, Side_t::Buy)) == 2);
    CHECK(c.tail_ram(A(0, 0, p + 1, Side_t::Buy)) == 0);

    const Delta d = delta({{0, 4}});
    c.take_line(A(0, 0, p, Side_t::Buy), d.vec, d.sum);
    CHECK(c.tail_ram(A(0, 0, p, Side_t::Buy)) == 2);

    const CancelResult r = c.cancel_slot(A(0, 1, p, Side_t::Buy), 2);
    CHECK(r.found);
    CHECK(c.tail_ram(A(0, 0, p, Side_t::Buy)) == 2);
}

TEST_CASE("head line hint points at the first line and invalidates on drain") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x10;
    for (Pos_t pos = 0; pos < 16; ++pos) {
        c.insert_slot(A(0, pos, p, Side_t::Sell), pos + 1, 1);
    }
    c.insert_slot(A(0, 16, p, Side_t::Sell), 100, 1);

    const IDVol_key_t first_key = get_IDVol_key(IDVolCacheIndexing(A(0, 0, p, Side_t::Sell)));
    CHECK(c.meta_head_line(A(0, 0, p, Side_t::Sell)) == first_key);

    const VolLine consume_all = full_line(1);
    c.take_line(A(0, 0, p, Side_t::Sell), consume_all, 16);

    // The level still holds volume on line 1, but the head hint for the drained
    // line is invalidated and must be re-established by the scanner.
    CHECK(c.meta_head_line(A(0, 0, p, Side_t::Sell)) == std::nullopt);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Sell)) == 1);

    const IDVol_key_t second_key = get_IDVol_key(IDVolCacheIndexing(A(0, 16, p, Side_t::Sell)));
    c.set_meta_head_line(A(0, 0, p, Side_t::Sell), second_key);
    CHECK(c.meta_head_line(A(0, 0, p, Side_t::Sell)) == second_key);
}

TEST_CASE("line-sum and level-sum rows expose 16 adjacent entries") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x10;
    c.insert_slot(A(0, 0, p, Side_t::Sell), 1, 5);
    c.insert_slot(A(0, 1, p, Side_t::Sell), 2, 3);
    c.insert_slot(A(0, 2, p, Side_t::Sell), 3, 4);
    c.insert_slot(A(0, 0, p + 5, Side_t::Sell), 4, 7);

    const auto line_row = c.read_line_sum_row(A(0, 0, p, Side_t::Sell));
    CHECK(line_row[0] == 12); // first id-vol line (pos[7:4] == 0)
    CHECK(line_row[1] == 0);

    const auto level_row = c.read_level_sum_row(A(0, 0, p, Side_t::Sell));
    CHECK(level_row[0] == 12); // price 0x10 -> price[3:0] == 0
    CHECK(level_row[5] == 7);  // price 0x15 -> price[3:0] == 5
}

TEST_CASE("reset clears ram, sums, tail and meta") {
    IdVolMap c;
    c.reset();
    const Price_t p = 0x10;
    c.insert_slot(A(0, 0, p, Side_t::Sell), 1, 5);
    c.insert_slot(A(0, 1, p, Side_t::Sell), 2, 3);
    const Delta d = delta({{0, 2}});
    c.take_line(A(0, 0, p, Side_t::Sell), d.vec, d.sum);
    REQUIRE(c.meta_head_line(A(0, 0, p, Side_t::Sell)).has_value());

    c.reset();

    CHECK(c.ram.empty());
    CHECK(c.line_sum.ram.empty());
    CHECK(c.level_sum.ram.empty());
    CHECK(c.tail.ram.empty());
    CHECK(c.slot_volume(A(0, 0, p, Side_t::Sell)) == 0);
    CHECK(c.line_sum_ram(A(0, 0, p, Side_t::Sell)) == 0);
    CHECK(c.level_sum_ram(A(0, 0, p, Side_t::Sell)) == 0);
    CHECK(c.tail_ram(A(0, 0, p, Side_t::Sell)) == 0);
    CHECK(c.meta_head_line(A(0, 0, p, Side_t::Sell)) == std::nullopt);

    // After reset the tail restarts from zero for the level.
    c.insert_slot(A(0, 0, p, Side_t::Sell), 9, 4);
    CHECK(c.tail_ram(A(0, 0, p, Side_t::Sell)) == 1);
}

TEST_CASE("idvol sums, tail and slots match a reference model under random ops") {
    IdVolMap c;
    c.reset();
    const Price_t price = 0x120;
    const Side_t side = Side_t::Sell;
    constexpr size_t kMaxPos = 48; // three id-vol lines

    std::array<Qty_t, kMaxPos> vol{};
    std::array<OrderID_t, kMaxPos> oid{};
    Pos_t tail = 0;
    OrderID_t next_id = 1;

    auto ref_line_sum = [&](size_t pos) {
        const size_t base = (pos >> 4) << 4;
        QtySum_t s = 0;
        for (size_t i = base; i < base + 16; ++i) {
            s += vol[i];
        }
        return s;
    };
    auto ref_level_sum = [&] {
        QtySum_t s = 0;
        for (Qty_t v : vol) {
            s += v;
        }
        return s;
    };

    std::mt19937_64 rng(0x9E3779B97F4A7C15ULL);
    for (int step = 0; step < 4000; ++step) {
        const uint64_t pick = rng() % 3;
        if (pick == 0 && tail < kMaxPos) {
            const Qty_t v = static_cast<Qty_t>(1 + rng() % 5);
            c.insert_slot(A(0, tail, price, side), next_id, v);
            vol[tail] = v;
            oid[tail] = next_id;
            ++next_id;
            ++tail;
        } else if (pick == 1) {
            std::vector<size_t> live_lines;
            for (size_t base = 0; base < kMaxPos; base += 16) {
                if (ref_line_sum(base) != 0) {
                    live_lines.push_back(base);
                }
            }
            if (live_lines.empty()) {
                continue;
            }
            const size_t base = live_lines[rng() % live_lines.size()];
            const Qty_t budget = static_cast<Qty_t>(1 + rng() % 10);
            Qty_t consumed = 0;
            VolLine dv{};
            for (size_t off = 0; off < 16 && consumed < budget; ++off) {
                const Qty_t take = std::min<Qty_t>(budget - consumed, vol[base + off]);
                dv[off] = take;
                vol[base + off] -= take;
                consumed += take;
            }
            if (consumed == 0) {
                continue;
            }
            c.take_line(A(0, base, price, side), dv, consumed);
        } else {
            std::vector<size_t> live;
            for (size_t i = 0; i < kMaxPos; ++i) {
                if (vol[i] != 0) {
                    live.push_back(i);
                }
            }
            if (live.empty()) {
                continue;
            }
            const size_t pos = live[rng() % live.size()];
            const Qty_t removed = vol[pos];
            const CancelResult r = c.cancel_slot(A(0, pos, price, side), oid[pos]);
            REQUIRE(r.found);
            CHECK(r.order_id == oid[pos]);
            CHECK(r.volume_removed == removed);
            vol[pos] = 0;
        }

        CHECK(c.tail_ram(A(0, 0, price, side)) == tail);
        CHECK(c.level_sum_ram(A(0, 0, price, side)) == ref_level_sum());
        for (size_t base = 0; base < kMaxPos; base += 16) {
            CHECK(c.line_sum_ram(A(0, base, price, side)) == ref_line_sum(base));
        }
        if (step % 8 == 0) {
            for (size_t i = 0; i < kMaxPos; ++i) {
                CHECK(c.slot_volume(A(0, i, price, side)) == vol[i]);
            }
        }
        size_t nonzero_lines = 0;
        for (size_t base = 0; base < kMaxPos; base += 16) {
            if (ref_line_sum(base) != 0) {
                ++nonzero_lines;
            }
        }
        CHECK(c.ram.size() == nonzero_lines);
    }
}
