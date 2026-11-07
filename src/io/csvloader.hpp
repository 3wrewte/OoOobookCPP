#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/instructions.hpp"
#include "core/addr.hpp"
#include "core/types.hpp"
#include "core/registers.hpp"

// ---------------------------------------------------------------------------
// InstructionsFromCSV
//
// Reads a crosscheck-style order file:
//
//   seq,op,order_id,user_id,contract_id,side,price,qty,order_type
//   1,N,1,1001,1,buy,10000,7,limit
//   2,N,2,1002,1,sell,10002,5,limit
//   3,C,1,1001,1,,,,
//
// Column aliases mirror old/crates/exchange-csv: op/action/type/cmd,
// order_id/client_order_id/id, user_id/account_id/account, contract_id/contract,
// side/buy_sell/bs, qty/quantity/size/amount, order_type/otype/ord_type.
// Missing optional columns fall back to the Rust defaults: seq = data row
// number, user_id = 0, order_id = data row number, contract_id = 1,
// order_type = limit.
//
// Prices are integer ticks (crosscheck price_scale = 1). qty must fit u32 and
// price must fit the 28-bit address field; anything else throws with the file
// line number, so a malformed fixture fails at load time instead of deep in the
// pipeline.
//
// reset() loads the whole file into `instructions`. issue(ready_num) is the
// tick handshake: it returns at most `ready_num` contexts and advances
// `current` by the same amount. Like any Register writer it must be called
// during Phase::Comb.
// ---------------------------------------------------------------------------

namespace csvio {

inline std::string trim(std::string s) {
    const auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && is_ws(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_ws(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

inline std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string strip_bom(std::string s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

inline std::string normalize(const std::string& name) {
    std::string s = lower_ascii(trim(strip_bom(name)));
    for (char& c : s) {
        if (c == '-') c = '_';
    }
    return s;
}

// Minimal CSV split: handles quoted fields and doubled quotes, ignores '\r'.
inline std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cur.push_back(c);
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

inline bool is_new(const std::string& token) {
    const std::string v = lower_ascii(trim(token));
    return v == "new" || v == "add" || v == "insert" || v == "n" || v == "a" || v == "0";
}

inline bool is_cancel(const std::string& token) {
    const std::string v = lower_ascii(trim(token));
    return v == "cancel" || v == "delete" || v == "remove" || v == "c" || v == "d" ||
           v == "x" || v == "1";
}

inline std::uint64_t parse_u64(const std::string& s, const char* what, std::size_t line_no) {
    if (s.empty()) {
        throw std::runtime_error("InstructionsFromCSV: line " + std::to_string(line_no) +
                                 ": missing " + what);
    }
    if (s.front() == '-') {
        throw std::runtime_error("InstructionsFromCSV: line " + std::to_string(line_no) +
                                 ": negative " + what + " '" + s + "'");
    }
    std::size_t pos = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(s, &pos, 10);
    } catch (const std::exception&) {
        throw std::runtime_error("InstructionsFromCSV: line " + std::to_string(line_no) +
                                 ": invalid " + what + " '" + s + "'");
    }
    if (pos != s.size()) {
        throw std::runtime_error("InstructionsFromCSV: line " + std::to_string(line_no) +
                                 ": invalid " + what + " '" + s + "'");
    }
    return static_cast<std::uint64_t>(value);
}

inline Side_t parse_side(const std::string& s, std::size_t line_no) {
    const std::string v = lower_ascii(trim(s));
    if (v == "buy" || v == "bid" || v == "b" || v == "1") return Side_t::Buy;
    if (v == "sell" || v == "ask" || v == "s" || v == "2" || v == "-1") return Side_t::Sell;
    throw std::runtime_error("InstructionsFromCSV: line " + std::to_string(line_no) +
                             ": invalid side '" + s + "'");
}

} // namespace csvio

struct InstructionsFromCSV{
    std::vector<OPcontext> instructions;
    std::string filename;
    Register<size_t> current;

    explicit InstructionsFromCSV(std::string name) : filename(std::move(name)) {}

    void reset();
    void prepare();
    void commit();

    size_t get_valid_num() const {
        const size_t cur = current.get();
        return cur < instructions.size() ? instructions.size() - cur : 0;
    }

    std::vector<OPcontext> issue(size_t ready_num) {
        const size_t base = current.get();
        const size_t num = std::min(get_valid_num(), ready_num); // handshake
        current.set(base + num);
        return std::vector<OPcontext>(instructions.begin() + base,
                                      instructions.begin() + base + num);
    }
};

inline void InstructionsFromCSV::reset(){
    current.reset();
    instructions.clear();

    std::ifstream in(filename);
    if (!in.is_open()) {
        throw std::runtime_error("InstructionsFromCSV: cannot open '" + filename + "'");
    }

    std::string line;
    if (!std::getline(in, line)) {
        return; // header-only / empty file
    }

    const std::vector<std::string> header = csvio::split(line);
    std::unordered_map<std::string, std::size_t> column;
    for (std::size_t i = 0; i < header.size(); ++i) {
        column[csvio::normalize(header[i])] = i;
    }

    const auto find_column = [&](std::initializer_list<const char*> names) -> std::optional<std::size_t> {
        for (const char* name : names) {
            const auto it = column.find(csvio::normalize(name));
            if (it != column.end()) return it->second;
        }
        return std::nullopt;
    };

    const auto seq_col      = find_column({"seq"});
    const auto op_col       = find_column({"op", "action", "type", "cmd"});
    const auto id_col       = find_column({"order_id", "client_order_id", "cl_ord_id", "id"});
    const auto account_col  = find_column({"user_id", "account_id", "account", "uid"});
    const auto contract_col = find_column({"contract_id", "contract", "instrument_id", "symbol_id", "symbol"});
    const auto side_col     = find_column({"side", "buy_sell", "bs"});
    const auto price_col    = find_column({"price", "px"});
    const auto qty_col      = find_column({"qty", "quantity", "size", "amount"});
    const auto type_col     = find_column({"order_type", "otype", "ord_type"});

    std::size_t line_no = 1;
    std::size_t row_index = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (csvio::trim(line).empty()) continue;
        ++row_index;

        const std::vector<std::string> fields = csvio::split(line);
        const auto field = [&](std::optional<std::size_t> col) -> std::string {
            if (!col || *col >= fields.size()) return std::string();
            return csvio::trim(fields[*col]);
        };

        // Missing/empty action defaults to "new", matching the Rust parser.
        const std::string action = op_col ? field(op_col) : std::string();
        const bool cancel = !action.empty() && csvio::is_cancel(action);
        const bool add    = action.empty() || csvio::is_new(action);
        if (!cancel && !add) {
            throw std::runtime_error("InstructionsFromCSV: line " + std::to_string(line_no) +
                                     ": unknown op '" + action + "'");
        }

        OPcontext ctx{};
        ctx.Seq           = seq_col ? csvio::parse_u64(field(seq_col), "seq", line_no)
                                    : static_cast<std::uint64_t>(row_index);
        ctx.AccountId     = account_col ? csvio::parse_u64(field(account_col), "user_id", line_no) : 0;
        ctx.ClientOrderId = id_col ? csvio::parse_u64(field(id_col), "order_id", line_no)
                                   : static_cast<std::uint64_t>(row_index);

        const std::uint64_t contract = contract_col
                                          ? csvio::parse_u64(field(contract_col), "contract_id", line_no)
                                          : 1u;
        if (contract >= 128u) {
            throw std::runtime_error("InstructionsFromCSV: line " + std::to_string(line_no) +
                                     ": contract_id " + std::to_string(contract) +
                                     " does not fit the 7-bit sym field");
        }
        ctx.ContractId = static_cast<std::uint32_t>(contract);

        if (cancel) {
            ctx.kind  = OPKind::Cancel;
            ctx.Side  = Side_t::Buy; // unused for cancels
            ctx.Price = 0;
            ctx.qty   = 0;
        } else {
            const std::string order_type = type_col ? csvio::lower_ascii(field(type_col)) : std::string();
            const bool market = (order_type == "market" || order_type == "m" || order_type == "2");
            ctx.kind = market ? OPKind::Market : OPKind::Limit;
            ctx.Side = csvio::parse_side(field(side_col), line_no);

            const std::uint64_t qty = csvio::parse_u64(field(qty_col), "qty", line_no);
            if (qty > static_cast<std::uint64_t>(Qty_max)) {
                throw std::runtime_error("InstructionsFromCSV: line " + std::to_string(line_no) +
                                         ": qty " + std::to_string(qty) + " does not fit 32 bits");
            }
            ctx.qty = static_cast<Qty_t>(qty);

            if (market) {
                ctx.Price = 0;
            } else {
                const std::uint64_t price = csvio::parse_u64(field(price_col), "price", line_no);
                if (price > static_cast<std::uint64_t>(Price_max)) {
                    throw std::runtime_error("InstructionsFromCSV: line " + std::to_string(line_no) +
                                             ": price " + std::to_string(price) +
                                             " does not fit the 28-bit address field");
                }
                ctx.Price = static_cast<Price_t>(price);
            }
        }

        instructions.push_back(ctx);
    }
}

inline void InstructionsFromCSV::prepare(){
    current.prepare();
}

inline void InstructionsFromCSV::commit(){
    current.commit();
}
