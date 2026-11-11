#pragma once
#include <vector>

#include "core/types.hpp"
#include "core/instructions.hpp"
#include "core/wire.hpp"
#include "bookkeeping/IDallocator.hpp"

#include "core/events.hpp"

struct router{
    uint64_t trade_count_  = 0;
    QtySum_t total_volume_ = 0;
    Price_t  last_price_   = 0;
    Qty_t    last_qty_     = 0;
    Qty_t    fill_count_   = 0;

    wire<bool> trade_tick_event_lock;

    void reset(){
        trade_count_  = 0;
        total_volume_ = 0;
        last_price_   = 0;
        last_qty_     = 0;
        fill_count_   = 0;
        trade_tick_event_lock.reset();
    }
    void prepare(){
        trade_tick_event_lock.prepare();
    }
    void commit(){
        trade_tick_event_lock.commit();
    }



    std::tuple<std::vector<TradeEvent>, std::vector<TickEvent>, std::vector<InputReplyEvent> >
        RouteEvents(const std::vector<CanonicalEvent> & events, const IDallocator &idallocator) // for trade stream, tick stream, and market order auto capping
    {
        trade_tick_event_lock.set(true); // call only once, prevent multi count of trade
        std::vector<TradeEvent> trade_events;
        std::vector<TickEvent >  tick_events;
        std::vector<InputReplyEvent> reply_events;
        for(auto &i : events){
            const auto taker_it = idallocator.map_out.find(i.id);
            assert (taker_it != idallocator.map_out.end()); // since the order is not retired yet, it must exists in map_out
            const OPcontext &taker = taker_it->second;
            if(i.kind == UopKind::TakeLine){
                const auto result = std::get<TakeLineResult>(i.payload);
                for(size_t j = 0; j < 16; j++){
                    if(!result.trade_vol_vec[j])continue;
                    const auto maker_it = idallocator.map_out.find(result.trade_id_vec[j]);
                    assert (maker_it != idallocator.map_out.end());
                    const OPcontext & maker = maker_it->second;
                    trade_events.push_back(TradeEvent{
                        .seq                   = taker.Seq                    ,
                        .trade_idx             = ++trade_count_               ,
                        .taker_client_order_id = taker.ClientOrderId          ,
                        .maker_client_order_id = maker.ClientOrderId          ,
                        .taker_side            = taker.Side                   ,
                        .price                 = maker.Price                  ,
                        .qty                   = result.trade_vol_vec[j]      ,
                        .maker_cleared         = result.trade_complete_vec[j]
                    });
                    //trade_count_  += 1                      ;
                    total_volume_ += result.trade_vol_vec[j];
                    last_price_    = maker.Price            ;
                    last_qty_      = result.trade_vol_vec[j];
                    fill_count_   += result.trade_vol_vec[j];
                }
            }
            if(i.op_end){
                const Qty_t final_fill = fill_count_;
                fill_count_ = 0;
                assert(i.snapshot.has_value());
                const CanonicalBookSnapshot snapshot = i.snapshot.value();
                tick_events.push_back(TickEvent {
                    .seq          = taker.Seq                     ,
                    .bid_price    = snapshot.bid_price.value_or(0), // 0 when the bid side is empty
                    .bid_qty      = snapshot.bid_qty              ,
                    .ask_price    = snapshot.ask_price.value_or(0), // 0 when the ask side is empty
                    .ask_qty      = snapshot.ask_qty              ,
                    .last_price   = last_price_                   , // 0 before the first trade
                    .last_qty     = last_qty_                     ,
                    .trade_count  = trade_count_                  ,
                    .total_volume = total_volume_                 ,
                });
                const auto order_it = idallocator.map_out.find(i.id);
                assert (order_it != idallocator.map_out.end()); // since the order is not retired yet, it must exists in map_out
                const OPcontext &order = order_it->second;
                const bool rejected = (i.kind == UopKind::End)
                    && (std::get<OPIssue>(i.payload) != OPIssue::Default);
                reply_events.push_back(InputReplyEvent {
                    .seq            = order.Seq,
                    .contract_id    = order.ContractId,
                    .account_id     = order.AccountId,
                    .client_order_id= order.ClientOrderId,
                    .reject_reason  = (i.kind == UopKind::End)? std::get<OPIssue>(i.payload) : OPIssue::Default,
                    .reject_detail  = 0, // not done yet
                    .filled_qty     = final_fill,
                    .rested_qty     = (!rejected && order.kind==OPKind::Limit  && i.kind==UopKind::InsertSlot)? order.qty - final_fill : 0,
                    .discarded_qty  = (!rejected && order.kind==OPKind::Market)?order.qty - final_fill : 0,   // discarded 用于 market 剩余
                    .cancel_found   = (i.kind == UopKind::CancelSlot)? std::get<CancelResult>(i.payload).found : false,
                    .cancel_qty     = (i.kind == UopKind::CancelSlot)? std::get<CancelResult>(i.payload).volume_removed : 0,
                });
                }
            }
        }
        return {trade_events, tick_events, reply_events};
    }

    std::vector<InputReplyEvent> InputReplyEvents(const std::vector<CanonicalEvent> &events, const IDallocator &idallocator){
        std::vector<InputReplyEvent> reply_events;
        for(auto &i : events){
            if(i.op_end){
                const auto order_it = idallocator.map_out.find(i.id);
                assert (order_it != idallocator.map_out.end()); // since the order is not retired yet, it must exists in map_out
                const OPcontext &order = order_it->second;
                reply_events.push_back(InputReplyEvent {
                    .seq            = order.Seq,
                    .contract_id    = order.ContractId,
                    .account_id     = order.AccountId,
                    .client_order_id= order.ClientOrderId,
                    .reject_reason  = (i.kind == UopKind::End)? std::get<OPIssue>(i.payload) : OPIssue::Default,
                    .reject_detail  = 0, // not done yet
                    //.filled_qty     = ,
                    //.rested_qty     = ,
                    //.discarded_qty  = ,   // discarded 用于 market 剩余
                    .cancel_found   = (i.kind == UopKind::Cancel)? std::get<CancelResult>(i.payload).found : false,
                    .cancel_qty     = (i.kind == UopKind::Cancel)? std::get<CancelResult>(i.payload).volume_removed : 0,
                });
            }
        }
        return reply_events;
    }

};
