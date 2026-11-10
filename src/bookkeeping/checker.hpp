#pragma once
#include <map>
#include <vector>
#include <unordered_map>
#include "core/instructions.hpp"
#include "core/addr.hpp"
#include "core/types.hpp"

struct AccountState {
    std::optional<int64_t> cash;               // 已结算的 quote 余额（含 locked） nullopt for infinite money
    int64_t cash_locked;                       // 挂单/在途买单锁定的 quote（最坏成本 + fee buffer）
    std::optional<int64_t> base;               // 已结算的 base 持仓（含 locked） nullopt for infinifte base
    int64_t base_locked;                       // 挂单/在途卖单锁定的 base
    std::optional<int64_t>     position_limit; // base 上限（可选）nullopt for no upper bound
    // open_orders: internal_order_id -> {side, limit_price, remaining, reserved_cash/base}
    AccountState(){
        cash = std::nullopt;
        cash_locked = 0;
        base = std::nullopt;
        base_locked = 0;
        position_limit = std::nullopt;
    }
};

struct Checker{
    std::unordered_map<uint64_t, AccountState> states;
    //double market_reserve_rate;

    void reset();
    void reset(std::unordered_map<uint64_t, AccountState> &init_states, double rate);
    void prepare();
    void commit();

    OPcontext check(OPcontext &opc);
    void update(std::vector<CanonicalEvent> &events);
};


inline void Checker::reset()
{
    states = {};
    //market_reserve_rate = 0.05;
}
inline void Checker::reset(std::unordered_map<uint64_t, AccountState>& init_states, double rate)
{
    states = std::move(init_states);
    //market_reserve_rate = rate;
}
inline void Checker::prepare(){}
inline void Checker::commit(){}

inline OPcontext Checker::check(OPcontext &opc)
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

        if(opc.Side == Buy){
            if(states[opc.AccountId].cash.has_value()
                && uint64_t(opc.Price) * uint64_t(opc.qty) >
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


