#pragma once
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/events.hpp"
#include "core/types.hpp"

// ---------------------------------------------------------------------------
// CSV output modules.
//
// The router (`bookkeeping/router.hpp`) is the single consumer of canonical
// events. It emits self-contained `TradeEvent` / `TickEvent` records; these
// streams only format and buffer them. No order-context lookup happens here.
//
//   stream.record(const TradeEvent&)
//   stream.record(const std::vector<TradeEvent>&)
//
//   stream.record(const TickEvent&)
//   stream.record(const std::vector<TickEvent>&)
//
// The single-row `record(TradeRow)` / `record(TickRow)` overloads remain as an
// escape hatch for callers that already hold a formatted row.
//
// The two streams are separate classes: a trade stream never carries tick
// counters and a tick stream never writes a trade row, so neither object has a
// half-unused mode. They share only the file/buffer plumbing.
// ---------------------------------------------------------------------------

struct TradeRow {
    Seq      seq                    ;
    uint64_t trade_idx              ; // 1-based, global across the run
    uint64_t taker_client_order_id  ; // crosscheck "taker_order_id" column
    uint64_t maker_client_order_id  ; // crosscheck "maker_order_id" column
    Side_t   taker_side             ;
    Price_t  price                  ; // maker (resting) price
    Qty_t    qty                    ;
};

struct TickRow {
    Seq      seq                    ;
    Price_t  bid_price              ; // 0 when the bid side is empty
    QtySum_t bid_qty                ;
    Price_t  ask_price              ; // 0 when the ask side is empty
    QtySum_t ask_qty                ;
    Price_t  last_price             ; // 0 before the first trade
    Qty_t    last_qty               ;
    uint64_t trade_count            ;
    QtySum_t total_volume           ;
};

inline constexpr const char* TradeCSVHeader =
    "seq,trade_idx,taker_order_id,maker_order_id,taker_side,price,qty";
inline constexpr const char* TickCSVHeader =
    "seq,bid_price,bid_qty,ask_price,ask_qty,last_price,last_qty,trade_count,volume";

inline std::string format_trade_row(const TradeRow& row) {
    std::string line;
    line.reserve(64);
    line += std::to_string(row.seq);
    line += ',';
    line += std::to_string(row.trade_idx);
    line += ',';
    line += std::to_string(row.taker_client_order_id);
    line += ',';
    line += std::to_string(row.maker_client_order_id);
    line += ',';
    line.push_back(row.taker_side == Side_t::Buy ? 'B' : 'S');
    line += ',';
    line += std::to_string(row.price);
    line += ',';
    line += std::to_string(row.qty);
    return line;
}

inline std::string format_tick_row(const TickRow& row) {
    std::string line;
    line.reserve(96);
    line += std::to_string(row.seq);
    line += ',';
    line += std::to_string(row.bid_price);
    line += ',';
    line += std::to_string(row.bid_qty);
    line += ',';
    line += std::to_string(row.ask_price);
    line += ',';
    line += std::to_string(row.ask_qty);
    line += ',';
    line += std::to_string(row.last_price);
    line += ',';
    line += std::to_string(row.last_qty);
    line += ',';
    line += std::to_string(row.trade_count);
    line += ',';
    line += std::to_string(row.total_volume);
    return line;
}

// ---------------------------------------------------------------------------
// OutputStreamToCSV: buffered file.
//
//   reset()   -> open the file and write the header
//   prepare() -> clear the wire buffer
//   record()  -> append one formatted line
//   commit()  -> drain the wire buffer into the file
//   finish()  -> flush and close (call once at the end of the run)
// ---------------------------------------------------------------------------
class OutputStreamToCSV {
public:
    OutputStreamToCSV(std::string filename, std::string header)
        : filename_(std::move(filename)), header_(std::move(header)) {}

    virtual ~OutputStreamToCSV() = default;

    OutputStreamToCSV(const OutputStreamToCSV&) = delete;
    OutputStreamToCSV& operator=(const OutputStreamToCSV&) = delete;

    virtual void reset() {
        write_buffer_.clear();
        out_.open(filename_, std::ios::out | std::ios::trunc);
        if (!out_.is_open()) {
            throw std::runtime_error("OutputStreamToCSV: cannot open '" + filename_ + "'");
        }
        out_ << header_ << '\n';
    }

    void prepare() {
        write_buffer_.clear();
    }

    void commit() {
        if (!out_.is_open()) {
            throw std::runtime_error("OutputStreamToCSV: commit before reset on '" + filename_ + "'");
        }
        for (const std::string& line : write_buffer_) {
            out_ << line << '\n';
        }
    }

    void finish() {
        if (!out_.is_open()) {
            throw std::runtime_error("OutputStreamToCSV: finish before reset on '" + filename_ + "'");
        }
        out_.flush();
        out_.close();
    }

    void record(std::string line) {
        write_buffer_.push_back(std::move(line));
    }

protected:
    std::string filename_;
    std::string header_;
    std::vector<std::string> write_buffer_; // wire
    std::ofstream out_;
};

// ---------------------------------------------------------------------------
// TradeStreamToCSV
// ---------------------------------------------------------------------------
class TradeStreamToCSV : public OutputStreamToCSV {
public:
    explicit TradeStreamToCSV(std::string filename)
        : OutputStreamToCSV(std::move(filename), TradeCSVHeader) {}

    using OutputStreamToCSV::record;

    void record(const TradeRow& row) {
        OutputStreamToCSV::record(format_trade_row(row));
    }

    void record(const TradeEvent& ev) {
        TradeRow row{};
        row.seq                    = ev.seq;
        row.trade_idx              = ev.trade_idx;
        row.taker_client_order_id  = ev.taker_client_order_id;
        row.maker_client_order_id  = ev.maker_client_order_id;
        row.taker_side             = ev.taker_side;
        row.price                  = ev.price;
        row.qty                    = ev.qty;
        record(row);
    }

    void record(const std::vector<TradeEvent>& events) {
        for (const TradeEvent& ev : events) record(ev);
    }
};

// ---------------------------------------------------------------------------
// TickStreamToCSV
// ---------------------------------------------------------------------------
class TickStreamToCSV : public OutputStreamToCSV {
public:
    explicit TickStreamToCSV(std::string filename)
        : OutputStreamToCSV(std::move(filename), TickCSVHeader) {}

    using OutputStreamToCSV::record;

    void record(const TickRow& row) {
        OutputStreamToCSV::record(format_tick_row(row));
    }

    void record(const TickEvent& ev) {
        TickRow row{};
        row.seq          = ev.seq;
        row.bid_price    = ev.bid_price;
        row.bid_qty      = ev.bid_qty;
        row.ask_price    = ev.ask_price;
        row.ask_qty      = ev.ask_qty;
        row.last_price   = ev.last_price;
        row.last_qty     = ev.last_qty;
        row.trade_count  = ev.trade_count;
        row.total_volume = ev.total_volume;
        record(row);
    }

    void record(const std::vector<TickEvent>& events) {
        for (const TickEvent& ev : events) record(ev);
    }
};
