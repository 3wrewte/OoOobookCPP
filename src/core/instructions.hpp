#pragma once
#include <variant>
#include <optional>
#include "types.hpp"
#include "addr.hpp"


enum class OPKind : uint8_t { Limit = 0b10, Market = 0b11, Cancel = 0b01, NOOP = 0b00};

enum class OPIssue : uint32_t {
    Default            = 0x0000, // 正常NOOP
    InvalidPrice       = 0x8000, // price 越界/非法
    InvalidQty         = 0x8001, // qty==0
    QtyTooLarge        = 0x8002, // 超过上限
    InvalidContract    = 0x8003, // sym/contract 非法
    UnknownCancelTarget= 0x8004, // 撤单目标不存在（也可继续用 CancelResult.found=false）
    InsufficientFunds  = 0x8005, // checker: 现金/保证金不足
    PositionLimit      = 0x8006, // checker: 会超持仓上限
    PostOnlyWouldCross = 0x8007, // post-only 会吃单
    SelfTradePrevented = 0x8008, // 会和自己成交
    RiskLimit          = 0x8009, // 其他风险
    UnknownError       = 0xffff
};

struct OP {
    OrderID_t       id        ;
    OPKind          kind      ;
    Sym_t           sym       ;
    Side_t          side      ;
    uint32_t        imm0      ; // Limit: price, market: empty, Cancel: target_id, NOOP: OPIssue
    uint32_t        imm1      ; // Limit/market: qty, Cancel: empty, NOOP: detail 32 bit
};

struct OPcontext{
    OPKind          kind      ;
    uint64_t Seq;
    uint32_t ContractId;
    uint64_t AccountId;
    uint64_t ClientOrderId;
    //OrderID_t id;
    Side_t   Side;
    Price_t  Price;
    Qty_t    qty;
    OPIssue  reject_reason;
    uint32_t reject_detail;
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
    CanonicalAddr_t addr      ; // may use sym, side price instead Take/Insert: addr = Canonical addr(but only stores level information), Cancel: target_addr, End: (detail 32 bit)future
    uint32_t        imm       ; // Take: budget, Insert: volume, Cancel: target_id, End: OPIssue
};

enum class UopKind : uint8_t { TakeLine = 0b00, InsertSlot = 0b01, CancelSlot = 0b10, End = 0b11};

struct TakeLineResult{
    IDLine   trade_id_vec      ;
    VolLine  trade_vol_vec     ;
    MaskLine trade_complete_vec;
    Qty_t    consumed          ;
    //Qty_t    remaining_budget; // should always be zero
    QtySum_t line_remaining    ; // needed or not?
};

struct CancelResult {
    bool found;
    OrderID_t order_id;
    Qty_t volume_removed;
};

struct InsertLineResult{
    CanonicalAddr_t insert_addr;
    Qty_t insert_qty;
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
    std::variant<std::monostate, TakeLineResult, InsertLineResult, CancelResult, OPIssue> payload;
    /* payload: Take(traded_ids,take_vols,consumed) | Insert | Cancel | End */
    std::optional<CanonicalBookSnapshot> snapshot;
};

struct Uop {
    UopKind kind;
    OrderID_t       id        ;
    // MopSeq          parent_mop;
    OpSeq           op_seq    ;
    bool            op_end    ;
    CanonicalAddr_t addr      ; // Take: addr = line_key << line_bits, Insert/Cancel: addr, End: (detail 32 bit)future
    uint32_t        imm       ; // Take: budget, Insert: volume, Cancel: target_id, End: OPIssue
};


