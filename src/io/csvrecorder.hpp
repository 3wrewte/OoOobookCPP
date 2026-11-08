#pragma once
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/types.hpp"

// ---------------------------------------------------------------------------
// CSV output module.
//
// This is deliberately a *formatter*, not a reconstructor. The runtime owns the
// bookkeeping needed to turn canonical events into exchange-level rows:
//   - the taker's OP context (seq / side / client order id),
//   - the maker's client order id and resting price (IDallocator / resting map),
//   - the cumulative tick counters (trade_count / total_volume / last_*).
// Trying to derive those from `CanonicalEvent` alone loses the seq and the
// maker price, so the runtime fills a TradeRow / TickRow and the module only
// formats and buffers it.
//
// Module contract matches the rest of the project:
//   reset()   -> open the file and write the header (initialisation only)
//   prepare() -> clear the wire buffer
//   record()  -> append one formatted row to the wire buffer
//   commit()  -> drain the wire buffer into the file
//   finish()  -> flush and close (call once at the end of the run)
// ---------------------------------------------------------------------------

struct TradeRow {
    Seq      seq                    ;
    uint64_t trade_idx              ; // 1-based, global across the run
    uint64_t taker_client_order_id   ; // crosscheck "taker_order_id" column
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

struct OutputStreamToCSV{
    // treat the external file as a register
    std::string filename;
    std::string header;
    std::vector<std::string> write_buffer; // wire
    std::ofstream out;

    explicit OutputStreamToCSV(std::string name, std::string header_line)
        : filename(std::move(name)), header(std::move(header_line)) {}

    OutputStreamToCSV(const OutputStreamToCSV&) = delete;
    OutputStreamToCSV& operator=(const OutputStreamToCSV&) = delete;

    void reset  ();// open file
    void prepare();// clear all wires, that is, buffers
    void commit ();// output data to file
    void finish ();// flush and close

    //size_t get_ready_num() const {    } // for static_length record, not needed for now

    void record(std::string line) { write_buffer.push_back(std::move(line)); }
    void record(const TradeRow& row) { record(format_trade_row(row)); }
    void record(const TickRow&  row) { record(format_tick_row(row)); }
};

inline void OutputStreamToCSV::reset(){
    write_buffer.clear();
    out.open(filename, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        throw std::runtime_error("OutputStreamToCSV: cannot open '" + filename + "'");
    }
    out << header << '\n';
}

inline void OutputStreamToCSV::prepare(){
    write_buffer.clear();
}

inline void OutputStreamToCSV::commit(){
    if (!out.is_open()) {
        throw std::runtime_error("OutputStreamToCSV: commit before reset on '" + filename + "'");
    }
    for (const std::string& line : write_buffer) {
        out << line << '\n';
    }
}

inline void OutputStreamToCSV::finish(){
    if (!out.is_open()) {
        throw std::runtime_error("OutputStreamToCSV: finish before reset on '" + filename + "'");
    }
    out.flush();
    out.close();
}
