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
    const OrderID_t id = (opc.kind != OPKind::Cancel)? count : 0;
    if(opc.kind != OPKind::Cancel){
        count++;
        map_out[id] = opc;
        map_in[{opc.AccountId, opc.ClientOrderId}] = id;
    }
    if(opc.ContractId >=128)return NOOP;
    uint32_t imm0, imm1;
    if(opc.kind == OPKind::Limit){
        if(opc.qty >= Price_max)return NOOP;
        imm0 = opc.Price;
        imm1 = opc.qty;
    }
    else if(opc.kind == OPKind::Market){
        if(opc.qty >= Price_max)return NOOP;
        imm0 = 0;
        imm1 = opc.qty;
    }
    else if(opc.kind == OPKind::Cancel){
        if (map_in.find({opc.AccountId, opc.ClientOrderId}) == map_in.end()) return NOOP;
        imm0 = map_in[{opc.AccountId, opc.ClientOrderId}];
        imm1 = 0;
    }
    return OP{
        .id = id,
        .kind = opc.kind,
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
