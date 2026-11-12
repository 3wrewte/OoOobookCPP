#pragma once
#include <map>
#include <vector>
#include <unordered_map>
#include "core/instructions.hpp"
#include "core/addr.hpp"
#include "core/types.hpp"
#include "core/events.hpp"

struct AccountState {
    std::optional<uint64_t> cash;               // 已结算的 quote 余额（含 locked） nullopt for infinite money
    uint64_t cash_locked;                       // 挂单/在途买单锁定的 quote（最坏成本 + fee buffer）
    std::optional<uint64_t> base;               // 已结算的 base 持仓（含 locked） nullopt for infinifte base
    uint64_t base_locked;                       // 挂单/在途卖单锁定的 base
    std::optional<uint64_t>     position_limit; // base 上限（可选）nullopt for no upper bound
    // open_orders: internal_order_id -> {side, limit_price, remaining, reserved_cash/base}
    AccountState(){
        cash = std::nullopt;
        cash_locked = 0;
        base = std::nullopt;
        base_locked = 0;
        position_limit = std::nullopt;
    }
};

struct ReservedOrder{
    Side_t  side           ;
    Price_t price          ;// price for limit, cap for market
    Qty_t qty              ;
    //Qty_t remaining        ;
    //uint64_t reserved_cash ;
    //Qty_t reserved_position;
    uint64_t account       ;
};

struct Checker{
    std::unordered_map<AccountId_t, AccountState> states;
    //double market_reserve_rate;
    std::map<std::pair<AccountId_t, ClientOrderId_t>, ReservedOrder> open; // {AccountId, ClientOrderId} -> reserved order, exists for every non-retired market and limit

    void reset();
    void reset(std::unordered_map<uint64_t, AccountState> &init_states, double rate);
    void prepare();
    void commit();

    OPcontext check(const OPcontext &opc);
    void update(const std::vector<TradeEvent> &trade_events, const std::vector<InputReplyEvent> &reply_events);
};


inline void Checker::reset()
{
    states = {};
    //market_reserve_rate = 0.05;
}
inline void Checker::reset(std::unordered_map<uint64_t, AccountState>& init_states, double rate)
{
    (void)rate;
    states = std::move(init_states);
    //market_reserve_rate = rate;
}
inline void Checker::prepare(){}
inline void Checker::commit(){}

inline OPcontext Checker::check(const OPcontext &opc)
{
    if(states.find(opc.AccountId) == states.end()){
            return OPcontext{
                .kind         = OPKind::NOOP,
                .Seq          = opc.Seq,
                .ContractId   = opc.ContractId,
                .AccountId    = opc.AccountId,
                .ClientOrderId= opc.ClientOrderId,
                .Side         = opc.Side,
                .Price        = opc.Price,
                .qty          = opc.qty,
                .reject_reason= OPIssue::InvalidAccount,
                .reject_detail= 0
            };
        }
    if(opc.ContractId >= 128)return OPcontext{
            .kind         = OPKind::NOOP,
            .Seq          = opc.Seq,
            .ContractId   = opc.ContractId,
            .AccountId    = opc.AccountId,
            .ClientOrderId= opc.ClientOrderId,
            .Side         = opc.Side,
            .Price        = opc.Price,
            .qty          = opc.qty,
            .reject_reason= OPIssue::InvalidContract,
            .reject_detail= 0
        };
    if(opc.kind == OPKind :: Limit || opc.kind == OPKind :: Market){
        if(opc.Price > Price_max)return OPcontext{
            .kind         = OPKind::NOOP,
            .Seq          = opc.Seq,
            .ContractId   = opc.ContractId,
            .AccountId    = opc.AccountId,
            .ClientOrderId= opc.ClientOrderId,
            .Side         = opc.Side,
            .Price        = opc.Price,
            .qty          = opc.qty,
            .reject_reason= OPIssue::InvalidPrice,
            .reject_detail= 0
        };
        if(opc.qty == 0 || opc.qty > Qty_max)return OPcontext{
            .kind         = OPKind::NOOP,
            .Seq          = opc.Seq,
            .ContractId   = opc.ContractId,
            .AccountId    = opc.AccountId,
            .ClientOrderId= opc.ClientOrderId,
            .Side         = opc.Side,
            .Price        = opc.Price,
            .qty          = opc.qty,
            .reject_reason= OPIssue::InvalidQty,
            .reject_detail= 0
        };
        if(open.find({opc.AccountId, opc.ClientOrderId}) != open.end())return OPcontext{
            .kind         = OPKind::NOOP,
            .Seq          = opc.Seq,
            .ContractId   = opc.ContractId,
            .AccountId    = opc.AccountId,
            .ClientOrderId= opc.ClientOrderId,
            .Side         = opc.Side,
            .Price        = opc.Price,
            .qty          = opc.qty,
            .reject_reason= OPIssue::RepeatedID,
            .reject_detail= 0
        };

        if(opc.Side == Buy){
            const uint64_t reserved_cash_calc = uint64_t(opc.Price) * uint64_t(opc.qty);
            if(states[opc.AccountId].cash.has_value()
                &&  reserved_cash_calc >
                std::saturating_sub(states[opc.AccountId].cash.value_or(0) , states[opc.AccountId].cash_locked))
                return OPcontext{
                    .kind         = OPKind::NOOP,
                    .Seq          = opc.Seq,
                    .ContractId   = opc.ContractId,
                    .AccountId    = opc.AccountId,
                    .ClientOrderId= opc.ClientOrderId,
                    .Side         = opc.Side,
                    .Price        = opc.Price,
                    .qty          = opc.qty,
                    .reject_reason= OPIssue::InsufficientFunds,
                    .reject_detail= 0
                };
            if(states[opc.AccountId].base.has_value() && states[opc.AccountId].position_limit.has_value()
                && opc.qty + states[opc.AccountId].base.value_or(0) > states[opc.AccountId].position_limit.value_or(0))
                return OPcontext{
                    .kind         = OPKind::NOOP,
                    .Seq          = opc.Seq,
                    .ContractId   = opc.ContractId,
                    .AccountId    = opc.AccountId,
                    .ClientOrderId= opc.ClientOrderId,
                    .Side         = opc.Side,
                    .Price        = opc.Price,
                    .qty          = opc.qty,
                    .reject_reason= OPIssue::PositionLimit,
                    .reject_detail= 0
                };
            open[{opc.AccountId, opc.ClientOrderId}] = ReservedOrder{
                .side              = Buy               ,
                .price             = opc.Price         ,
                .qty               = opc.qty           ,
                //.remaining         = opc.qty           ,
                //.reserved_cash     = reserved_cash_calc,
                //.reserved_position = 0                 ,
                .account           = opc.AccountId
            };
            states[opc.AccountId].cash_locked +=reserved_cash_calc;
            return opc;
        }
        else if(opc.Side == Sell){
            if(states[opc.AccountId].base.has_value()
                && opc.qty  >
                std::saturating_sub(states[opc.AccountId].base.value_or(0), states[opc.AccountId].base_locked))
                return OPcontext{
                    .kind         = OPKind::NOOP,
                    .Seq          = opc.Seq,
                    .ContractId   = opc.ContractId,
                    .AccountId    = opc.AccountId,
                    .ClientOrderId= opc.ClientOrderId,
                    .Side         = opc.Side,
                    .Price        = opc.Price,
                    .qty          = opc.qty,
                    .reject_reason= OPIssue::InsufficientBase,
                    .reject_detail= 0
                };
            open[{opc.AccountId, opc.ClientOrderId}] = ReservedOrder{
                .side              = Sell              ,
                .price             = opc.Price         ,
                .qty               = opc.qty           ,
                //.remaining         = opc.qty           ,
                //.reserved_cash     = 0                 ,
                //.reserved_position = opc.qty           ,
                .account           = opc.AccountId
            };
            states[opc.AccountId].base_locked += opc.qty;
            return opc;
        }
    }
    else if(opc.kind == OPKind::Cancel){
        return opc;
    }
    else if(opc.kind == OPKind::NOOP){
        return OPcontext{
            .kind         = OPKind::NOOP,
            .Seq          = opc.Seq,
            .ContractId   = opc.ContractId,
            .AccountId    = opc.AccountId,
            .ClientOrderId= opc.ClientOrderId,
            .Side         = opc.Side,
            .Price        = opc.Price,
            .qty          = opc.qty,
            .reject_reason= opc.reject_reason,
            .reject_detail= opc.reject_detail
        };
    }
    return OPcontext{
        .kind         = OPKind::NOOP,
        .Seq          = opc.Seq,
        .ContractId   = opc.ContractId,
        .AccountId    = opc.AccountId,
        .ClientOrderId= opc.ClientOrderId,
        .Side         = opc.Side,
        .Price        = opc.Price,
        .qty          = opc.qty,
        .reject_reason= OPIssue::UnknownError,
        .reject_detail= 0
    };
}



inline void Checker::update(const std::vector<TradeEvent> &trade_events, const std::vector<InputReplyEvent> &reply_events){
    for (auto &e : trade_events){
        assert(open.find({e.taker_account_id, e.taker_client_order_id}) != open.end());
        assert(open.find({e.maker_account_id, e.maker_client_order_id}) != open.end());
        const ReservedOrder &taker = open.find({e.taker_account_id, e.taker_client_order_id})->second;
        const ReservedOrder &maker = open.find({e.maker_account_id, e.maker_client_order_id})->second;
        const ReservedOrder &buyer =  (e.taker_side == Buy)? taker : maker;
        const ReservedOrder &seller = (e.taker_side == Buy)? maker : taker;

        uint64_t total_price = uint64_t(e.price) * uint64_t(e.qty);

        if(states[buyer.account].cash.has_value())
            states[buyer.account].cash = states[buyer.account].cash.value() - total_price;
        if(states[buyer.account].base.has_value())
            states[buyer.account].base = states[buyer.account].base.value() + uint64_t(e.qty);
        states[buyer.account].cash_locked -= uint64_t(e.qty) * uint64_t(buyer.price); // use reserved price instead of trade price

        if(states[seller.account].cash.has_value())
            states[seller.account].cash = std::saturating_add( states[seller.account].cash.value() , total_price);
        if(states[seller.account].base.has_value())
            states[seller.account].base = states[seller.account].base.value() - uint64_t(e.qty);
        states[seller.account].base_locked -= uint64_t(e.qty);

        //buyer.remaining -= e.qty;
        //buyer.reserved_cash -= uint64_t(e.qty) * uint64_t(buyer.price);

        //seller.remaining -= e.qty;
        //seller.reserved_position -= e.qty;


        if(e.maker_cleared){
            //assert(maker.remaining == 0);
            open.erase({e.maker_account_id, e.maker_client_order_id});
        }
    }


    for (auto &e : reply_events){
        if(e.kind == OPKind::Market){
            assert(open.find({e.account_id, e.client_order_id}) != open.end());
            ReservedOrder &record = open.find({e.account_id, e.client_order_id})->second;
            //assert(record.remaining == e.discarded_qty);
            if(record.side == Buy)
                states[record.account].cash_locked -= uint64_t(e.discarded_qty) * uint64_t(record.price);
            else if(record.side == Sell)
                states[record.account].base_locked -= uint64_t(e.discarded_qty);
            open.erase({e.account_id, e.client_order_id});
        }
        else if(e.kind == OPKind::Limit){
            if(!e.rested_qty)open.erase({e.account_id, e.client_order_id}); // may rest and taken in one tick, hence may not be in open_map now
        }
        else if(e.kind == OPKind::Cancel){
            if(e.cancel_found){
                assert(open.find({e.account_id, e.client_order_id}) != open.end());
                ReservedOrder &record = open.find({e.account_id, e.client_order_id})->second;
                //assert(record.remaining == e.cancel_qty);
                if(record.side == Buy)
                    states[record.account].cash_locked -= uint64_t(e.cancel_qty) * uint64_t(record.price);
                else if(record.side == Sell)
                    states[record.account].base_locked -= uint64_t(e.cancel_qty);
                open.erase({e.account_id, e.client_order_id});
            }
        }
    }
}
