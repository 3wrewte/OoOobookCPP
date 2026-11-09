#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "io/csvrecorder.hpp"

namespace {

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

CanonicalEvent take_event(OrderID_t id,
                          OpSeq op_seq,
                          std::initializer_list<std::pair<OrderID_t, Qty_t>> trades) {
    TakeLineResult tr{};
    std::size_t slot = 0;
    Qty_t consumed = 0;
    for (const auto& [maker_id, vol] : trades) {
        tr.trade_id_vec[slot] = maker_id;
        tr.trade_vol_vec[slot] = vol;
        consumed += vol;
        ++slot;
    }
    tr.consumed = consumed;
    tr.line_remaining = 0;
    return CanonicalEvent{
        .kind = UopKind::TakeLine,
        .id = id,
        .op_seq = op_seq,
        .op_end = false,
        .payload = tr,
        .snapshot = std::nullopt};
}

CanonicalEvent end_event(OrderID_t id, OpSeq op_seq, CanonicalBookSnapshot snapshot) {
    return CanonicalEvent{
        .kind = UopKind::End,
        .id = id,
        .op_seq = op_seq,
        .op_end = true,
        .payload = std::monostate{},
        .snapshot = snapshot};
}

OPcontext taker(uint64_t seq, uint64_t client, Side_t side) {
    OPcontext ctx{};
    ctx.Seq = seq;
    ctx.ClientOrderId = client;
    ctx.Side = side;
    return ctx;
}

OPcontext maker(uint64_t client, Price_t price) {
    OPcontext ctx{};
    ctx.ClientOrderId = client;
    ctx.Price = price;
    return ctx;
}

OrderLookup lookup_of(std::initializer_list<std::pair<OrderID_t, OPcontext>> items) {
    std::map<OrderID_t, OPcontext> table(items.begin(), items.end());
    return [table](OrderID_t id) -> std::optional<OPcontext> {
        const auto it = table.find(id);
        if (it == table.end()) return std::nullopt;
        return it->second;
    };
}

} // namespace

TEST_CASE("csv recorder formats a trade row") {
    TradeRow row{};
    row.seq = 7;
    row.trade_idx = 3;
    row.taker_client_order_id = 101;
    row.maker_client_order_id = 202;
    row.taker_side = Side_t::Sell;
    row.price = 10000;
    row.qty = 5;
    CHECK(format_trade_row(row) == "7,3,101,202,S,10000,5");
}

TEST_CASE("csv recorder formats a tick row") {
    TickRow row{};
    row.seq = 7;
    row.bid_price = 10000;
    row.bid_qty = 12;
    row.ask_price = 10005;
    row.ask_qty = 9;
    row.last_price = 10000;
    row.last_qty = 5;
    row.trade_count = 3;
    row.total_volume = 42;
    CHECK(format_tick_row(row) == "7,10000,12,10005,9,10000,5,3,42");

    TickRow empty{};
    empty.seq = 1;
    CHECK(format_tick_row(empty) == "1,0,0,0,0,0,0,0,0");
}

TEST_CASE("trade stream reconstructs rows from canonical events") {
    const std::string path = "build/trade_stream.csv";
    const std::vector<CanonicalEvent> events = {
        take_event(101, 0, {{202, 5}}),
        end_event(101, 0, CanonicalBookSnapshot{}),
    };
    const auto lookup = lookup_of({
        {101, taker(1, 101, Side_t::Buy)},
        {202, maker(202, 10000)},
    });

    TradeStreamToCSV out(path);
    out.reset();
    out.prepare();
    out.record(events.size(), events, lookup);
    out.commit();
    out.finish();

    CHECK(read_all(path) == std::string(TradeCSVHeader) + "\n1,1,101,202,B,10000,5\n");
}

TEST_CASE("trade stream numbers consecutive trades across OPs") {
    const std::string path = "build/trade_stream_multi.csv";
    const std::vector<CanonicalEvent> events = {
        take_event(101, 0, {{202, 3}}),
        end_event(101, 0, CanonicalBookSnapshot{}),
        take_event(102, 1, {{203, 2}, {204, 1}}),
        end_event(102, 1, CanonicalBookSnapshot{}),
    };
    const auto lookup = lookup_of({
        {101, taker(1, 101, Side_t::Buy)},
        {102, taker(2, 102, Side_t::Sell)},
        {202, maker(202, 10000)},
        {203, maker(203, 10001)},
        {204, maker(204, 10002)},
    });

    TradeStreamToCSV out(path);
    out.reset();
    out.prepare();
    out.record(events.size(), events, lookup);
    out.commit();
    out.finish();

    const std::string expected =
        std::string(TradeCSVHeader) + "\n" +
        "1,1,101,202,B,10000,3\n" +
        "2,2,102,203,S,10001,2\n" +
        "2,3,102,204,S,10002,1\n";
    CHECK(read_all(path) == expected);
}

TEST_CASE("trade stream carries pending trades across partial batches") {
    const std::string path = "build/trade_stream_partial.csv";
    const auto lookup = lookup_of({
        {101, taker(1, 101, Side_t::Buy)},
        {202, maker(202, 10000)},
    });

    TradeStreamToCSV out(path);
    out.reset();
    out.prepare();
    out.record(1, {take_event(101, 0, {{202, 5}})}, lookup); // no op_end yet
    out.record(1, {end_event(101, 0, CanonicalBookSnapshot{})}, lookup);
    out.commit();
    out.finish();

    CHECK(read_all(path) == std::string(TradeCSVHeader) + "\n1,1,101,202,B,10000,5\n");
}

TEST_CASE("tick stream reconstructs counters and snapshot") {
    const std::string path = "build/tick_stream.csv";
    CanonicalBookSnapshot snapshot{};
    snapshot.bid_price = 9999;
    snapshot.bid_qty = 10;
    snapshot.ask_price = 10005;
    snapshot.ask_qty = 7;

    const std::vector<CanonicalEvent> events = {
        take_event(101, 0, {{202, 5}}),
        end_event(101, 0, snapshot),
    };
    const auto lookup = lookup_of({
        {101, taker(1, 101, Side_t::Buy)},
        {202, maker(202, 10000)},
    });

    TickStreamToCSV out(path);
    out.reset();
    out.prepare();
    out.record(events.size(), events, lookup);
    out.commit();
    out.finish();

    CHECK(read_all(path) == std::string(TickCSVHeader) + "\n1,9999,10,10005,7,10000,5,1,5\n");
}

TEST_CASE("tick stream accumulates counters over multiple ticks") {
    const std::string path = "build/tick_stream_multi.csv";
    CanonicalBookSnapshot first{};
    first.bid_price = 9999;
    first.bid_qty = 10;
    CanonicalBookSnapshot second{};
    second.ask_price = 10005;
    second.ask_qty = 4;

    const auto lookup = lookup_of({
        {101, taker(1, 101, Side_t::Buy)},
        {102, taker(2, 102, Side_t::Sell)},
        {202, maker(202, 10000)},
        {203, maker(203, 10003)},
    });

    TickStreamToCSV out(path);
    out.reset();

    {
        const std::vector<CanonicalEvent> events = {
            take_event(101, 0, {{202, 5}}),
            end_event(101, 0, first),
        };
        out.prepare();
        out.record(events.size(), events, lookup);
        out.commit();
    }
    {
        const std::vector<CanonicalEvent> events = {
            take_event(102, 1, {{203, 2}}),
            end_event(102, 1, second),
        };
        out.prepare();
        out.record(events.size(), events, lookup);
        out.commit();
    }
    out.finish();

    const std::string expected =
        std::string(TickCSVHeader) + "\n" +
        "1,9999,10,0,0,10000,5,1,5\n" +
        "2,0,0,10005,4,10003,2,2,7\n";
    CHECK(read_all(path) == expected);
}

TEST_CASE("single-row escape hatch still works") {
    const std::string path = "build/trade_stream_single.csv";
    TradeRow row{};
    row.seq = 3;
    row.trade_idx = 1;
    row.taker_client_order_id = 1;
    row.maker_client_order_id = 2;
    row.taker_side = Side_t::Sell;
    row.price = 10;
    row.qty = 4;

    TradeStreamToCSV out(path);
    out.reset();
    out.prepare();
    out.record(row);
    out.commit();
    out.finish();

    CHECK(read_all(path) == std::string(TradeCSVHeader) + "\n3,1,1,2,S,10,4\n");
}

TEST_CASE("trade stream rejects an unknown maker") {
    const std::vector<CanonicalEvent> events = {
        take_event(101, 0, {{999, 5}}),
        end_event(101, 0, CanonicalBookSnapshot{}),
    };
    const auto lookup = lookup_of({{101, taker(1, 101, Side_t::Buy)}});

    TradeStreamToCSV out("build/trade_stream_unknown.csv");
    out.reset();
    out.prepare();
    CHECK_THROWS_AS(out.record(events.size(), events, lookup), std::runtime_error);
}

TEST_CASE("trade stream rejects a missing taker context") {
    const std::vector<CanonicalEvent> events = {
        end_event(999, 0, CanonicalBookSnapshot{}),
    };
    TradeStreamToCSV out("build/trade_stream_no_taker.csv");
    out.reset();
    out.prepare();
    CHECK_THROWS_AS(out.record(events.size(), events, lookup_of({})), std::runtime_error);
}

TEST_CASE("output stream rejects use before reset") {
    TradeStreamToCSV out("build/output_unused.csv");
    CHECK_THROWS_AS(out.commit(), std::runtime_error);
    CHECK_THROWS_AS(out.finish(), std::runtime_error);
}

TEST_CASE("output stream rejects an unwritable path") {
    TradeStreamToCSV out("build/no_such_dir/trades.csv");
    CHECK_THROWS_AS(out.reset(), std::runtime_error);
}
