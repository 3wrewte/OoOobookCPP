#pragma once
#include <map>
#include <unordered_map>
#include "core/instructions.hpp"
#include "core/addr.hpp"
#include "core/types.hpp"



struct IDallocator{
    std::map<std::pair<uint64_t, uint64_t>, OrderID_t> map_in;
    std::map<OrderID_t, OPcontext> map_out;
    uint32_t count;
    void reset();
    void prepare();
    void commit();

    OP allocate(OPcontext &opc);
    std::optional<OPcontext> get(OrderID_t id);
};
inline void IDallocator::reset(){
    map_in = {};
    map_out= {};
    count = 1; // avoid id 0
}
inline void IDallocator::prepare(){}
inline void IDallocator::commit(){}

inline OP IDallocator::allocate(OPcontext &opc){
    // One unique op id per input row so the output stage can always recover the
    // taker context, including Cancel / no-op rows.
    const OrderID_t op_id = count++;
    //opc.id = op_id;

    uint32_t imm0 = 0, imm1 = 0;
    OPKind kind = opc.kind;

    if (opc.ContractId >= 128) {
        kind = OPKind::NOOP;
    } else if (opc.kind == OPKind::Limit) {
        if (opc.Price > Price_max) {
            kind = OPKind::NOOP;
        } else {
            imm0 = opc.Price;
            imm1 = opc.qty;
        }
    } else if (opc.kind == OPKind::Market) {
        imm0 = 0;
        imm1 = opc.qty;
    } else if (opc.kind == OPKind::Cancel) {
        const auto it = map_in.find({opc.AccountId, opc.ClientOrderId});
        if (it == map_in.end()) {
            kind = OPKind::NOOP;
            imm0 = uint32_t(OPIssue::UnknownCancelTarget);
        } else {
            imm0 = it->second;
        }
    } else if (opc.kind == OPKind::NOOP) { // already rejected by checker
        kind = OPKind::NOOP;
        imm0 = uint32_t(opc.reject_reason);
    } else {
        kind = OPKind::NOOP;
        imm0 = uint32_t (OPIssue::UnknownError);
    }

    map_out[op_id] = opc; // context for the output stage (Cancel / NOOP included)
    if (kind == OPKind::Limit || kind == OPKind::Market) {
        map_in[{opc.AccountId, opc.ClientOrderId}] = op_id;
    }

    return OP{
        .id = op_id,
        .kind = kind,
        .sym = uint8_t(opc.ContractId),
        .side = opc.Side,
        .imm0 = imm0,
        .imm1 = imm1
    };
}
inline std::optional<OPcontext> IDallocator::get(OrderID_t id){
    if (map_out.find(id) == map_out.end()) return std::nullopt;
    return map_out[id];
}
