#pragma once
#include <vector>

#include "core/types.hpp"
#include "core/instructions.hpp"
#include "bookkeeping/IDallocator.hpp"

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
    Seq      seq                    ;
    //contract_id,
    uint64_t account_id     ;
    uint64_t client_order_id;
    //status: Rested / PartiallyFilled / Filled / Cancelled / Rejected / Noop,
    OPIssue reject_reason   ;
    uint32_t reject_detail  ;
    Qty_t filled_qty        ;
    Qty_t rested_qty        ;
    Qty_t discarded_qty     ;   // discarded 用于 market 剩余
    bool cancel_found       ;
    Qty_t cancel_qty        ;

};

struct router{

    void reset(){}
    void prepare(){}
    void commit(){}

    std::vector<OpSeq> EndEvents(const std::vector<CanonicalEvent> & events) // FOR op queue
    {
        std::vector<OpSeq> res;
        for(auto &i : events){
            if(i.op_end){
                res.push_back(i.op_seq);
            }
        }
        return res;
    }

    std::vector<NewRetireEvent> NewRetireEvents(const std::vector<CanonicalEvent> & events) // for resting index and ID map in/out
    {
        std::vector<NewRetireEvent> res;
        for(auto &i : events){
            if(i.kind == UopKind::InsertSlot){
                auto result = std::get<InsertLineResult>(i.payload);
                if(result.insert_qty){
                    res.push_back(NewRetireEvent{
                        .type = NewRetireEvent::Types::New,
                        .id   = i.id,
                        .addr = result.insert_addr
                    });
                }
            }
            else if(i.kind == UopKind::TakeLine){
                auto result = std::get<TakeLineResult>(i.payload);
                for(size_t j = 0; j < 16; j++){
                    if(result.trade_complete_vec[j]){
                        res.push_back(NewRetireEvent{
                            .type = NewRetireEvent::Types::Retire,
                            .id   = result.trade_id_vec[j],
                            .addr = std::nullopt
                        });
                    }
                }
            }
            else if(i.kind == UopKind::CancelSlot){
                auto result = std::get<CancelResult>(i.payload);
                if(result.found){
                    res.push_back(NewRetireEvent{
                        .type = NewRetireEvent::Types::Retire,
                        .id   = result.order_id,
                        .addr = std::nullopt
                    });
                }
            }
        }
        return res;
    }

    std::vector<TradeEvent> TradeEvents(const std::vector<CanonicalEvent> & events, IDallocator &idallocator) // for trade stream
    {
        std::vector<TradeEvent> res;
        for(auto &i : events){
            if(i.kind == UopKind::TakeLine){
                auto result = std::get<TakeLineResult>(i.payload);
                for(size_t j = 0; j < 16; j++){
                    res.push_back(TradeEvent{
                        //.seq,
                        //.trade_idx,
                        .taker_client_order_id = idallocator.map_out[i.id].AccountId,
                        .maker_client_order_id = idallocator.map_out[result.trade_id_vec[j]].AccountId,
                        .taker_side            = Side_t(! bool(idallocator.map_out[result.trade_id_vec[j]].Side)),
                        .price                 = idallocator.map_out[result.trade_id_vec[j]].Price,
                        .qty                   = result.consumed,
                        .maker_cleared         = result.trade_complete_vec[j]
                    });
                }
            }
        }
        return res;
    }


};
