#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <set>
#include <vector>

#include "src/core/pricemeta.hpp"
#include "src/core/types.hpp"

// Ports of the Rust `meta.rs` tests, plus boundary coverage for the current
// three-level active-price summaries. The C++ `BookMeta` exposes `reset()`
// instead of Rust's `BookMeta::new()`, and `sync_level_after_write` requires a
// concrete tail position instead of an Option, so the helper below supplies
// the same neutral values Rust's `sync_level` uses.

namespace {

void sync_level(BookMeta& m, Side_t side, Price_t price, QtySum_t old_sum,
                QtySum_t new_sum) {
    m.sync_level_after_write(side, price, old_sum, new_sum, 0,
                             new_sum > 0 ? 1 : 0, 0);
}

size_t side_index(Side_t side) {
    return static_cast<size_t>(side);
}

std::optional<Price_t> expected_next_ask(const std::set<Price_t>& active,
                                         Price_t p, Price_t limit) {
    auto it = active.upper_bound(p);
    if (it != active.end() && *it <= limit) return *it;
    return std::nullopt;
}

std::optional<Price_t> expected_next_bid(const std::set<Price_t>& active,
                                         Price_t p, Price_t limit) {
    auto it = active.lower_bound(p);
    if (it == active.begin()) return std::nullopt;
    --it;
    if (*it >= limit) return *it;
    return std::nullopt;
}

} // namespace

TEST_CASE("book meta best tracks active prices") {
    BookMeta m;
    m.reset();
    CHECK(m.best_ask() == std::nullopt);
    CHECK(m.best_bid() == std::nullopt);

    sync_level(m, Side_t::Sell, 0x12, 0, 5);
    sync_level(m, Side_t::Sell, 0x10, 0, 3);
    sync_level(m, Side_t::Buy, 0x0F, 0, 4);
    sync_level(m, Side_t::Buy, 0x0E, 0, 1);

    CHECK(m.best_ask() == 0x10);
    CHECK(m.best_bid() == 0x0F);

    sync_level(m, Side_t::Sell, 0x10, 3, 0);
    CHECK(m.best_ask() == 0x12);

    sync_level(m, Side_t::Sell, 0x12, 5, 0);
    CHECK(m.best_ask() == std::nullopt);
}

TEST_CASE("book meta next ask stops at limit") {
    BookMeta m;
    m.reset();
    sync_level(m, Side_t::Sell, 0x10, 0, 1);
    sync_level(m, Side_t::Sell, 0x12, 0, 1);
    sync_level(m, Side_t::Sell, 0x14, 0, 1);

    CHECK(m.next_ask_after(0x10, 0xFF) == 0x12);
    CHECK(m.next_ask_after(0x12, 0x13) == std::nullopt);
    CHECK(m.next_ask_after(0x14, 0xFF) == std::nullopt);
}

TEST_CASE("book meta next bid stops at limit") {
    BookMeta m;
    m.reset();
    sync_level(m, Side_t::Buy, 0x20, 0, 1);
    sync_level(m, Side_t::Buy, 0x1E, 0, 1);
    sync_level(m, Side_t::Buy, 0x1C, 0, 1);

    CHECK(m.next_bid_below(0x20, 0x00) == 0x1E);
    CHECK(m.next_bid_below(0x1E, 0x1D) == std::nullopt);
    CHECK(m.next_bid_below(0x1C, 0x00) == std::nullopt);
}

TEST_CASE("book meta summaries jump across empty rows blocks and super blocks") {
    BookMeta m;
    m.reset();
    const Price_t prices[] = {0x0000'0001, 0x0000'0410, 0x0001'0020, 0x0010'0000};
    for (Price_t price : prices) {
        sync_level(m, Side_t::Sell, price, 0, 1);
    }

    CHECK(m.best_ask() == 0x0000'0001);
    CHECK(m.next_ask_after(0x0000'0001, Price_max) == 0x0000'0410);
    CHECK(m.next_ask_after(0x0000'0410, Price_max) == 0x0001'0020);
    CHECK(m.next_ask_after(0x0001'0020, Price_max) == 0x0010'0000);
}

TEST_CASE("book meta price meta retains tail after level drains") {
    BookMeta m;
    m.reset();
    m.sync_level_after_write(Side_t::Buy, 0x10, 0, 7, 3, 7, 49);
    const PriceMeta meta = m.get_price_meta(Side_t::Buy, 0x10);
    CHECK(meta.tail_pos == 49);
    CHECK(meta.head_line == 3);
    CHECK(meta.head_valid);
    CHECK(meta.level_sum == 7);

    m.sync_level_after_write(Side_t::Buy, 0x10, 7, 0, 3, 0, 49);
    const PriceMeta retained = m.get_price_meta(Side_t::Buy, 0x10);
    CHECK(retained.tail_pos == 49);
    CHECK_FALSE(retained.active);
    CHECK_FALSE(retained.head_valid);
    CHECK(retained.level_sum == 0);
}

TEST_CASE("book meta head invalidates when current head line drains") {
    BookMeta m;
    m.reset();
    m.sync_level_after_write(Side_t::Sell, 0x10, 0, 10, 5, 10, 90);
    CHECK(m.get_meta_head_line(Side_t::Sell, 0x10) == 5);

    m.sync_level_after_write(Side_t::Sell, 0x10, 10, 4, 5, 0, 90);
    CHECK(m.get_meta_head_line(Side_t::Sell, 0x10) == std::nullopt);

    m.set_meta_head_line(Side_t::Sell, 0x10, 6);
    CHECK(m.get_meta_head_line(Side_t::Sell, 0x10) == 6);
}

TEST_CASE("book meta reset clears summaries and version") {
    BookMeta m;
    m.reset();
    sync_level(m, Side_t::Sell, 0x10, 0, 5);
    REQUIRE(m.best_ask() == 0x10);
    REQUIRE(m.get_version() != 0);

    m.reset();
    CHECK(m.best_ask() == std::nullopt);
    CHECK(m.best_bid() == std::nullopt);
    CHECK(m.get_version() == 0);
    CHECK(m.get_price_meta(Side_t::Sell, 0x10).active == false);
    CHECK(m.next_ask_after(0, Price_max) == std::nullopt);
}

TEST_CASE("book meta version changes only on active transitions") {
    BookMeta m;
    m.reset();
    CHECK(m.get_version() == 0);

    sync_level(m, Side_t::Sell, 0x10, 0, 5);
    CHECK(m.get_version() == 1);

    sync_level(m, Side_t::Sell, 0x10, 5, 7);
    CHECK(m.get_version() == 1);

    sync_level(m, Side_t::Sell, 0x10, 7, 0);
    CHECK(m.get_version() == 2);
}

TEST_CASE("book meta next ask is strict and limit inclusive") {
    BookMeta m;
    m.reset();
    sync_level(m, Side_t::Sell, 0x10, 0, 1);
    sync_level(m, Side_t::Sell, 0x12, 0, 1);

    CHECK(m.next_ask_after(0x10, Price_max) == 0x12);
    CHECK(m.next_ask_after(0x10, 0x11) == std::nullopt);
    CHECK(m.next_ask_after(0x10, 0x12) == 0x12);
    CHECK(m.next_ask_after(0x0F, 0x10) == 0x10);
    CHECK(m.next_ask_after(0x10, 0x10) == std::nullopt);
    CHECK(m.next_ask_after(0x12, Price_max) == std::nullopt);
    CHECK(m.next_ask_after(0x13, Price_max) == std::nullopt);
}

TEST_CASE("book meta next bid is strict and limit inclusive") {
    BookMeta m;
    m.reset();
    sync_level(m, Side_t::Buy, 0x10, 0, 1);
    sync_level(m, Side_t::Buy, 0x12, 0, 1);

    CHECK(m.next_bid_below(0x12, 0) == 0x10);
    CHECK(m.next_bid_below(0x12, 0x11) == std::nullopt);
    CHECK(m.next_bid_below(0x12, 0x10) == 0x10);
    CHECK(m.next_bid_below(0x13, 0x12) == 0x12);
    CHECK(m.next_bid_below(0x12, 0x12) == std::nullopt);
    CHECK(m.next_bid_below(0x10, 0) == std::nullopt);
    CHECK(m.next_bid_below(0x0F, 0) == std::nullopt);
}

TEST_CASE("book meta finds later block after start block has no eligible row") {
    BookMeta m;
    m.reset();
    sync_level(m, Side_t::Sell, 0x10, 0, 1);
    sync_level(m, Side_t::Sell, 0x400, 0, 1);

    // Start block 0 has an active row below the strict start price; the search
    // must continue to block 1 instead of giving up.
    CHECK(m.next_ask_after(0x10, Price_max) == 0x400);
}

TEST_CASE("book meta finds earlier block after start block has no eligible row") {
    BookMeta m;
    m.reset();
    sync_level(m, Side_t::Buy, 0x10, 0, 1);
    sync_level(m, Side_t::Buy, 0x410, 0, 1);

    // 0x410 is above the start price. Start block 1 has no eligible row below
    // 0x402, so the search must continue to block 0.
    CHECK(m.next_bid_below(0x402, 0) == 0x10);
}

TEST_CASE("book meta row offset edges are inclusive") {
    BookMeta m;
    m.reset();
    sync_level(m, Side_t::Sell, 0x00, 0, 1);
    sync_level(m, Side_t::Sell, 0x0F, 0, 1);
    CHECK(m.next_ask_after(0x00, 0x0F) == 0x0F);
    CHECK(m.next_ask_after(0x00, 0x0E) == std::nullopt);
    CHECK(m.next_ask_after(0x0F, Price_max) == std::nullopt);

    m.reset();
    sync_level(m, Side_t::Buy, 0x00, 0, 1);
    sync_level(m, Side_t::Buy, 0x0F, 0, 1);
    CHECK(m.next_bid_below(0x0F, 0x00) == 0x00);
    CHECK(m.next_bid_below(0x0F, 0x01) == std::nullopt);
    CHECK(m.next_bid_below(0x00, 0x00) == std::nullopt);
}

TEST_CASE("book meta crosses block and super boundaries") {
    BookMeta m;
    m.reset();
    sync_level(m, Side_t::Sell, 0x03F0, 0, 1);
    sync_level(m, Side_t::Sell, 0x0400, 0, 1);
    CHECK(m.next_ask_after(0x03F0, Price_max) == 0x0400);

    m.reset();
    sync_level(m, Side_t::Sell, 0xFFFF, 0, 1);
    sync_level(m, Side_t::Sell, 0x1'0000, 0, 1);
    CHECK(m.next_ask_after(0xFFFF, Price_max) == 0x1'0000);

    m.reset();
    sync_level(m, Side_t::Buy, 0x0400, 0, 1);
    sync_level(m, Side_t::Buy, 0x03F0, 0, 1);
    CHECK(m.next_bid_below(0x0400, 0) == 0x03F0);

    m.reset();
    sync_level(m, Side_t::Buy, 0x1'0000, 0, 1);
    sync_level(m, Side_t::Buy, 0xFFFF, 0, 1);
    CHECK(m.next_bid_below(0x1'0000, 0) == 0xFFFF);
}

TEST_CASE("book meta deactivating one price keeps row siblings visible") {
    BookMeta m;
    m.reset();
    sync_level(m, Side_t::Sell, 0x10, 0, 1);
    sync_level(m, Side_t::Sell, 0x11, 0, 1);

    sync_level(m, Side_t::Sell, 0x10, 1, 0);
    CHECK(m.best_ask() == 0x11);
    CHECK(m.next_ask_after(0x10, Price_max) == 0x11);

    sync_level(m, Side_t::Sell, 0x11, 1, 0);
    CHECK(m.best_ask() == std::nullopt);
}

TEST_CASE("book meta deactivating one block keeps super siblings visible") {
    BookMeta m;
    m.reset();
    sync_level(m, Side_t::Sell, 0x10, 0, 1);
    sync_level(m, Side_t::Sell, 0x410, 0, 1);

    sync_level(m, Side_t::Sell, 0x10, 1, 0);
    CHECK(m.best_ask() == 0x410);
    CHECK(m.next_ask_after(0x10, Price_max) == 0x410);

    sync_level(m, Side_t::Sell, 0x410, 1, 0);
    CHECK(m.best_ask() == std::nullopt);
}

TEST_CASE("book meta removing a non-best price keeps best unchanged") {
    BookMeta m;
    m.reset();
    sync_level(m, Side_t::Buy, 0x20, 0, 1);
    sync_level(m, Side_t::Buy, 0x10, 0, 1);

    sync_level(m, Side_t::Buy, 0x10, 1, 0);
    CHECK(m.best_bid() == 0x20);

    sync_level(m, Side_t::Buy, 0x20, 1, 0);
    CHECK(m.best_bid() == std::nullopt);
}

TEST_CASE("book meta active-price summaries match a reference set") {
    BookMeta m;
    m.reset();

    std::array<std::set<Price_t>, 2> active;
    std::vector<Price_t> pool = {
        0, 1, 0xF, 0x10, 0x11, 0x3F, 0x40, 0x3F0, 0x3FF,
        0x400, 0x401, 0xFFF, 0x1000, 0xFFFF, 0x1'0000, 0x1'0001,
        Price_max,
    };

    std::mt19937_64 rng(0xC0FFEEULL);
    for (size_t i = 0; i < 32; ++i) {
        pool.push_back(static_cast<Price_t>(rng() & Price_max));
    }

    for (size_t step = 0; step < 2000; ++step) {
        const Side_t side = (rng() & 1) ? Side_t::Sell : Side_t::Buy;
        const Price_t price = pool[rng() % pool.size()];
        auto& levels = active[side_index(side)];

        const bool was_active = levels.contains(price);
        const QtySum_t old_sum = was_active ? 1 : 0;
        const QtySum_t new_sum = was_active ? 0 : 1;
        sync_level(m, side, price, old_sum, new_sum);
        if (was_active) {
            levels.erase(price);
        } else {
            levels.insert(price);
        }

        CHECK(m.best_ask() ==
              (active[side_index(Side_t::Sell)].empty()
                   ? std::nullopt
                   : std::optional<Price_t>(*active[side_index(Side_t::Sell)].begin())));
        CHECK(m.best_bid() ==
              (active[side_index(Side_t::Buy)].empty()
                   ? std::nullopt
                   : std::optional<Price_t>(*active[side_index(Side_t::Buy)].rbegin())));

        for (size_t q = 0; q < 3; ++q) {
            const Price_t p = pool[rng() % pool.size()];
            const Price_t limit =
                (rng() & 1) ? Price_max : pool[rng() % pool.size()];

            CHECK(m.next_ask_after(p, limit) ==
                  expected_next_ask(active[side_index(Side_t::Sell)], p, limit));
            CHECK(m.next_bid_below(p, limit) ==
                  expected_next_bid(active[side_index(Side_t::Buy)], p, limit));
        }
    }
}
