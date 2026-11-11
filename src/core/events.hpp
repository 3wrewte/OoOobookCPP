#pragma once
#include <optional>
#include <vector>

#include "core/types.hpp"
#include "core/instructions.hpp"
struct NewRetireEvent{
    enum class Types{New, Retire} type;
    OrderID_t id;
    std::optional<CanonicalAddr_t> addr;
    //std::optional<Qty_t> resting;
};

struct TradeEvent {
    Seq      seq                    ;
    uint64_t trade_idx              ; // 1-based, global across the run
    uint64_t taker_client_order_id  ; // crosscheck "taker_order_id" column
    uint64_t maker_client_order_id  ; // crosscheck "maker_order_id" column
    Side_t   taker_side             ;
    Price_t  price                  ; // maker (resting) price
    Qty_t    qty                    ;
    bool     maker_cleared          ;
};

struct TickEvent {
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

struct InputReplyEvent {
    Seq      seq            ;
    uint32_t contract_id    ;
    uint64_t account_id     ;
    uint64_t client_order_id;
    //status: Rested / PartiallyFilled / Filled / Cancelled / Rejected / Noop,
    OPIssue  reject_reason  ;
    uint32_t reject_detail  ;
    Qty_t    filled_qty     ;
    Qty_t    rested_qty     ;
    Qty_t    discarded_qty  ;   // discarded 用于 market 剩余
    bool     cancel_found   ;
    Qty_t    cancel_qty     ;
};

inline std::vector<OpSeq> EndEvents(const std::vector<CanonicalEvent> & events) // FOR op queue
{
    std::vector<OpSeq> res;
    for(auto &i : events){
        if(i.op_end){
            res.push_back(i.op_seq);
        }
    }
    return res;
}

inline std::vector<NewRetireEvent> NewRetireEvents(const std::vector<CanonicalEvent> & events) // for resting index and ID map in/out
{
    std::vector<NewRetireEvent> res;
    for(auto &i : events){
        if(i.kind == UopKind::InsertSlot){
            const auto result = std::get<InsertLineResult>(i.payload);
            if(result.insert_qty){
                res.push_back(NewRetireEvent{
                    .type = NewRetireEvent::Types::New,
                    .id   = i.id,
                    .addr = result.insert_addr
                });
            }
        }
        else if(i.kind == UopKind::TakeLine){
            const auto result = std::get<TakeLineResult>(i.payload);
            for(size_t j = 0; j < 16; j++){
                if(result.trade_complete_vec[j]){
                    res.push_back(NewRetireEvent{
                        .type = NewRetireEvent::Types::Retire,
                        .id   = result.trade_id_vec[j],
                        .addr = std::nullopt
                    });
                }
            }
            if(i.op_end) // taker also retires
                res.push_back(NewRetireEvent{
                    .type = NewRetireEvent::Types::Retire,
                    .id   = i.id                         ,
                    .addr = std::nullopt
                });
        }
        else if(i.kind == UopKind::CancelSlot){
            const auto result = std::get<CancelResult>(i.payload);
            if(result.found){
                res.push_back(NewRetireEvent{
                    .type = NewRetireEvent::Types::Retire,
                    .id   = result.order_id,
                    .addr = std::nullopt
                });
            }
            // op_end should always be true
            if(i.op_end)
                res.push_back(NewRetireEvent{
                    .type = NewRetireEvent::Types::Retire,
                    .id   = i.id,
                    .addr = std::nullopt
                });
        }
        else if(i.kind == UopKind::End){
            // op_end should always be
            if(i.op_end)
                res.push_back(NewRetireEvent{
                    .type = NewRetireEvent::Types::Retire,
                    .id   = i.id                         ,
                    .addr = std::nullopt
                });
        }
    }
    return res;
}
