#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "io/csvrecorder.hpp"

namespace {

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

TradeEvent trade(uint64_t seq, uint64_t idx, uint64_t taker_client,
                 uint64_t maker_client, Side_t side, Price_t price, Qty_t qty,
                 bool cleared = false) {
    TradeEvent ev{};
    ev.seq = seq;
    ev.trade_idx = idx;
    ev.taker_client_order_id = taker_client;
    ev.maker_client_order_id = maker_client;
    ev.taker_side = side;
    ev.price = price;
    ev.qty = qty;
    ev.maker_cleared = cleared;
    return ev;
}

TickEvent tick(uint64_t seq, Price_t bid_price, QtySum_t bid_qty,
               Price_t ask_price, QtySum_t ask_qty, Price_t last_price,
               Qty_t last_qty, uint64_t trade_count, QtySum_t total_volume) {
    TickEvent ev{};
    ev.seq = seq;
    ev.bid_price = bid_price;
    ev.bid_qty = bid_qty;
    ev.ask_price = ask_price;
    ev.ask_qty = ask_qty;
    ev.last_price = last_price;
    ev.last_qty = last_qty;
    ev.trade_count = trade_count;
    ev.total_volume = total_volume;
    return ev;
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

TEST_CASE("trade stream writes a TradeEvent") {
    const std::string path = "build/trade_stream.csv";
    TradeStreamToCSV out(path);
    out.reset();
    out.prepare();
    out.record(trade(1, 1, 101, 202, Side_t::Buy, 10000, 5, true));
    out.commit();
    out.finish();

    CHECK(read_all(path) == std::string(TradeCSVHeader) + "\n1,1,101,202,B,10000,5\n");
}

TEST_CASE("trade stream writes a batch of TradeEvents") {
    const std::string path = "build/trade_stream_multi.csv";
    const std::vector<TradeEvent> events = {
        trade(2, 2, 102, 203, Side_t::Sell, 10001, 2),
        trade(2, 3, 102, 204, Side_t::Sell, 10002, 1),
    };

    TradeStreamToCSV out(path);
    out.reset();
    out.prepare();
    out.record(events);
    out.commit();
    out.finish();

    const std::string expected =
        std::string(TradeCSVHeader) + "\n" +
        "2,2,102,203,S,10001,2\n" +
        "2,3,102,204,S,10002,1\n";
    CHECK(read_all(path) == expected);
}

TEST_CASE("tick stream writes a TickEvent") {
    const std::string path = "build/tick_stream.csv";
    TickStreamToCSV out(path);
    out.reset();
    out.prepare();
    out.record(tick(1, 9999, 10, 10005, 7, 10000, 5, 1, 5));
    out.commit();
    out.finish();

    CHECK(read_all(path) == std::string(TickCSVHeader) + "\n1,9999,10,10005,7,10000,5,1,5\n");
}

TEST_CASE("tick stream writes a batch of TickEvents") {
    const std::string path = "build/tick_stream_multi.csv";
    const std::vector<TickEvent> events = {
        tick(1, 9999, 10, 0, 0, 10000, 5, 1, 5),
        tick(2, 0, 0, 10005, 4, 10003, 2, 2, 7),
    };

    TickStreamToCSV out(path);
    out.reset();
    out.prepare();
    out.record(events);
    out.commit();
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

TEST_CASE("output stream rejects use before reset") {
    TradeStreamToCSV out("build/output_unused.csv");
    CHECK_THROWS_AS(out.commit(), std::runtime_error);
    CHECK_THROWS_AS(out.finish(), std::runtime_error);
}

TEST_CASE("output stream rejects an unwritable path") {
    TradeStreamToCSV out("build/no_such_dir/trades.csv");
    CHECK_THROWS_AS(out.reset(), std::runtime_error);
}
