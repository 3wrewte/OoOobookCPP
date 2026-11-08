#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "io/csvrecorder.hpp"

namespace {

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

TradeRow trade(uint64_t seq, uint64_t idx, uint64_t taker, uint64_t maker,
               Side_t side, Price_t price, Qty_t qty) {
    TradeRow row{};
    row.seq = seq;
    row.trade_idx = idx;
    row.taker_client_order_id = taker;
    row.maker_client_order_id = maker;
    row.taker_side = side;
    row.price = price;
    row.qty = qty;
    return row;
}

} // namespace

TEST_CASE("csv recorder formats a trade row") {
    const auto row = trade(7, 3, 101, 202, Side_t::Sell, 10000, 5);
    CHECK(format_trade_row(row) == "7,3,101,202,S,10000,5");

    const auto buy = trade(9, 4, 303, 404, Side_t::Buy, 9999, 1);
    CHECK(format_trade_row(buy) == "9,4,303,404,B,9999,1");
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

TEST_CASE("csv recorder writes buffered rows on commit") {
    const std::string path = "build/csvrecorder_trades.csv";
    OutputStreamToCSV out(path, TradeCSVHeader);

    out.reset();
    out.prepare();
    out.record(trade(1, 1, 11, 22, Side_t::Buy, 10000, 3));
    out.record(trade(1, 2, 11, 33, Side_t::Buy, 10000, 2));
    out.commit();
    out.finish();

    const std::string expected =
        std::string(TradeCSVHeader) + "\n" +
        "1,1,11,22,B,10000,3\n" +
        "1,2,11,33,B,10000,2\n";
    CHECK(read_all(path) == expected);
}

TEST_CASE("csv recorder clears the wire on prepare") {
    const std::string path = "build/csvrecorder_two_ticks.csv";
    OutputStreamToCSV out(path, TradeCSVHeader);

    out.reset();
    out.prepare();
    out.record(trade(1, 1, 1, 2, Side_t::Buy, 10, 1));
    out.commit();

    out.prepare(); // must drop the previous buffer, not append to it
    out.record(trade(2, 2, 3, 4, Side_t::Sell, 11, 1));
    out.commit();
    out.finish();

    const std::string expected =
        std::string(TradeCSVHeader) + "\n" +
        "1,1,1,2,B,10,1\n" +
        "2,2,3,4,S,11,1\n";
    CHECK(read_all(path) == expected);
}

TEST_CASE("csv recorder rejects use before reset") {
    OutputStreamToCSV out("build/csvrecorder_unused.csv", TradeCSVHeader);
    CHECK_THROWS_AS(out.commit(), std::runtime_error);
    CHECK_THROWS_AS(out.finish(), std::runtime_error);
}

TEST_CASE("csv recorder rejects an unwritable path") {
    OutputStreamToCSV out("build/csvrecorder_no_such_dir/trades.csv", TradeCSVHeader);
    CHECK_THROWS_AS(out.reset(), std::runtime_error);
}
