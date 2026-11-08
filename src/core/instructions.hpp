#pragma once
#include <variant>
#include <optional>
#include "types.hpp"
#include "addr.hpp"


enum class OPKind : uint8_t { Limit = 0b10, Market = 0b11, Cancel = 0b01, NOOP = 0b00};

struct OP {
    OrderID_t       id        ;
    OPKind          kind      ;
    Sym_t           sym       ;
    Side_t          side      ;
    uint32_t        imm0      ; // Limit: price, market: empty, Cancel: target_id
    uint32_t        imm1      ; // Limit/market: qty, Cancel: empty
};

struct OPcontext{
    OPKind          kind      ;
    uint64_t Seq;
    uint32_t ContractId;
    uint64_t AccountId;
    uint64_t ClientOrderId;
    OrderID_t id;
    Side_t   Side;
    Price_t  Price;
    Qty_t    qty;
};

constexpr OP NOOP = OP{
    .id = 0,
    .kind = OPKind::NOOP,
    .sym = 0,
    .side = Side_t(0),
    .imm0 = 0,
    .imm1 = 0
};


enum class MopKind : uint8_t { TakeLevel = 0b00, InsertLevel = 0b01, CancelLevel = 0b10, End = 0b11};

struct Mop {
    MopKind         kind      ;
    OrderID_t       id        ;
    OpSeq           op_seq    ;
    bool            op_end    ;
    CanonicalAddr_t addr      ; // may use sym, side price instead Take/Insert: addr = Canonical addr(but only stores level information), Cancel: target_addr, End: 0
    uint32_t        imm       ; // Take: budget, Insert: volume, Cancel: target_id, End: 0
};

enum class UopKind : uint8_t { TakeLine = 0b00, InsertSlot = 0b01, CancelSlot = 0b10, End = 0b11};

struct TakeLineResult{
    IDLine   trade_id_vec    ;
    VolLine  trade_vol_vec   ;
    Qty_t    consumed        ;
    //Qty_t    remaining_budget; // should always be zero
    QtySum_t line_remaining  ; // needed or not?
};

struct CancelResult {
    bool found;
    OrderID_t order_id;
    Qty_t volume_removed;
};

struct CanonicalBookSnapshot {
    std::optional<Price_t> bid_price;
    std::optional<Price_t> ask_price;
    QtySum_t               bid_qty  ;
    QtySum_t               ask_qty  ;
};

struct CanonicalEvent {
    UopKind kind;
    OrderID_t id;
    OpSeq op_seq;
    //MopSeq parent_mop;
    //UopSeq uop_seq;
    bool op_end;
    std::variant<std::monostate, TakeLineResult, CanonicalAddr_t, CancelResult> payload;
    /* payload: Take(traded_ids,take_vols,consumed) | Insert | Cancel | End */
    std::optional<CanonicalBookSnapshot> snapshot;
};

struct Uop {
    UopKind kind;
    OrderID_t       id        ;
    // MopSeq          parent_mop;
    OpSeq           op_seq    ;
    bool            op_end    ;
    CanonicalAddr_t addr      ; // Take: addr = line_key << line_bits, Insert/Cancel: addr, End: 0
    uint32_t        imm       ; // Take: budget, Insert: volume, Cancel: target_id, End: 0
};


