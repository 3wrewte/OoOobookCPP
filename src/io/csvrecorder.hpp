#pragma once
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/instructions.hpp"
#include "core/types.hpp"

// ---------------------------------------------------------------------------
// CSV output modules.
//
// Layering:
//   - `OutputStreamToCSV` is the buffered file (a base class only).
//   - `TradeStreamToCSV`  turns canonical events into trades.csv rows.
//   - `TickStreamToCSV`   turns canonical events into ticks.csv rows.
//
// The main entry is:
//
//   stream.record(valid_num, events, lookup);
//
//   events  canonical events in uOP commit order (this tick)
//   lookup  internal order id -> OPcontext; the runtime passes
//           `[&](OrderID_t id){ return bookkeeping.get(id); }`
//
// `lookup` is used twice:
//   - at `op_end`, with `event.id`, to get the taker's seq / side / client id;
//   - for every non-zero take slot, with the maker id, to get the maker's
//     client id and resting price.
//
// The single-row `record(TradeRow)` / `record(TickRow)` overloads are kept as an
// escape hatch for tests and for callers that already reconstructed the row.
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

// The only dependency on bookkeeping: a lookup for an order id.
using OrderLookup = std::function<std::optional<OPcontext>(OrderID_t)>;

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

    void reset() override {
        OutputStreamToCSV::reset();
        next_trade_id_ = 1;
        pending_.clear();
    }

    using OutputStreamToCSV::record;

    void record(const TradeRow& row) {
        OutputStreamToCSV::record(format_trade_row(row));
    }

    // Consume the first `valid_num` events; emit one row per non-zero take
    // slot, at each op_end, using the taker context from `lookup(event.id)`.
    void record(std::size_t valid_num,
                const std::vector<CanonicalEvent>& events,
                const OrderLookup& lookup) {
        const std::size_t n = std::min(valid_num, events.size());

        for (std::size_t i = 0; i < n; ++i) {
            const CanonicalEvent& ev = events[i];

            if (ev.kind == UopKind::TakeLine) {
                const auto& take = std::get<TakeLineResult>(ev.payload);
                for (std::size_t slot = 0; slot < take.trade_vol_vec.size(); ++slot) {
                    if (take.trade_vol_vec[slot] != 0) {
                        pending_.push_back(PendingTrade{take.trade_id_vec[slot],
                                                        take.trade_vol_vec[slot]});
                    }
                }
            }

            if (ev.op_end) {
                const auto taker = lookup(ev.id);
                if (!taker) {
                    throw std::runtime_error(
                        "TradeStreamToCSV: no context for op id " + std::to_string(ev.id));
                }

                for (const PendingTrade& trade : pending_) {
                    const auto maker = lookup(trade.maker_id);
                    if (!maker) {
                        throw std::runtime_error(
                            "TradeStreamToCSV: unknown maker order id " +
                            std::to_string(trade.maker_id));
                    }
                    TradeRow row{};
                    row.seq = taker->Seq;
                    row.trade_idx = next_trade_id_++;
                    row.taker_client_order_id = taker->ClientOrderId;
                    row.maker_client_order_id = maker->ClientOrderId;
                    row.taker_side = taker->Side;
                    row.price = maker->Price;
                    row.qty = trade.volume;
                    record(row);
                }
                pending_.clear();
            }
        }
    }

private:
    struct PendingTrade {
        OrderID_t maker_id;
        Qty_t volume;
    };

    uint64_t next_trade_id_ = 1;
    std::vector<PendingTrade> pending_; // trades of an OP split across batches
};

// ---------------------------------------------------------------------------
// TickStreamToCSV
// ---------------------------------------------------------------------------
class TickStreamToCSV : public OutputStreamToCSV {
public:
    explicit TickStreamToCSV(std::string filename)
        : OutputStreamToCSV(std::move(filename), TickCSVHeader) {}

    void reset() override {
        OutputStreamToCSV::reset();
        trade_count_ = 0;
        total_volume_ = 0;
        last_price_ = 0;
        last_qty_ = 0;
    }

    using OutputStreamToCSV::record;

    void record(const TickRow& row) {
        OutputStreamToCSV::record(format_tick_row(row));
    }

    // Update the cumulative counters from take slots, then emit one tick per
    // op_end from the event's book snapshot.
    void record(std::size_t valid_num,
                const std::vector<CanonicalEvent>& events,
                const OrderLookup& lookup) {
        const std::size_t n = std::min(valid_num, events.size());

        for (std::size_t i = 0; i < n; ++i) {
            const CanonicalEvent& ev = events[i];

            if (ev.kind == UopKind::TakeLine) {
                const auto& take = std::get<TakeLineResult>(ev.payload);
                for (std::size_t slot = 0; slot < take.trade_vol_vec.size(); ++slot) {
                    if (take.trade_vol_vec[slot] == 0) continue;
                    const auto maker = lookup(take.trade_id_vec[slot]);
                    if (!maker) {
                        throw std::runtime_error(
                            "TickStreamToCSV: unknown maker order id " +
                            std::to_string(take.trade_id_vec[slot]));
                    }
                    ++trade_count_;
                    total_volume_ += take.trade_vol_vec[slot];
                    last_price_ = maker->Price;
                    last_qty_ = take.trade_vol_vec[slot];
                }
            }

            if (ev.op_end) {
                const auto taker = lookup(ev.id);
                if (!taker) {
                    throw std::runtime_error(
                        "TickStreamToCSV: no context for op id " + std::to_string(ev.id));
                }

                TickRow row{};
                row.seq = taker->Seq;
                if (ev.snapshot) {
                    row.bid_price = ev.snapshot->bid_price.value_or(0);
                    row.bid_qty   = ev.snapshot->bid_qty;
                    row.ask_price = ev.snapshot->ask_price.value_or(0);
                    row.ask_qty   = ev.snapshot->ask_qty;
                }
                row.last_price   = last_price_;
                row.last_qty     = last_qty_;
                row.trade_count  = trade_count_;
                row.total_volume = total_volume_;
                record(row);
            }
        }
    }

private:
    uint64_t trade_count_ = 0;
    QtySum_t total_volume_ = 0;
    Price_t last_price_ = 0;
    Qty_t last_qty_ = 0;
};
