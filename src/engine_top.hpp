
#pragma once

#include "core/instructions.hpp"
#include "core/addr.hpp"
#include "core/types.hpp"
#include "core/idvol.hpp"
#include "core/mop.hpp"
#include "core/op.hpp"
#include "core/phase.hpp"
#include "core/pricemeta.hpp"
#include "core/registers.hpp"
#include "core/sumcache.hpp"
#include "core/uop.hpp"
#include "core/wire.hpp"
#include "core/events.hpp"
#include "bookkeeping/IDallocator.hpp"
#include "bookkeeping/router.hpp"

struct engine_top{ // without checker
    IdVolMap idvol;
    OPQueue   op_queue;
    MopQueue mop_queue;
    UopQueue uop_queue;

    IDallocator id_allocator;
    router router_u;

    void reset  ();
    void prepare();
    void commit ();

    std::tuple<std::vector<TradeEvent>,std::vector<TickEvent>, std::vector<InputReplyEvent> >
    comb(size_t valid_num, std::vector<OPcontext>& OPcs); // input: valid_num, OPs
};

inline void engine_top::reset  (){
    idvol    .reset();
    op_queue.reset();
    mop_queue.reset();
    uop_queue.reset();
    id_allocator.reset();
    router_u.reset();
}
inline void engine_top::prepare(){
    //idvol; write-immediate, hence no prepare yet
    op_queue.prepare();
    mop_queue.prepare();
    uop_queue.prepare();
    id_allocator.prepare();
    router_u.prepare();
}
inline void engine_top::commit (){
    //idvol; write-immediate, hence no commit yet
    op_queue.commit();
    mop_queue.commit();
    uop_queue.commit();
    id_allocator.commit();
    router_u.commit();
}

std::tuple<std::vector<TradeEvent>,std::vector<TickEvent>, std::vector<InputReplyEvent> >
engine_top::comb(size_t op_valid_num, std::vector<OPcontext>& OPcs){
    std::vector<OP> ops;
    for(auto i : OPcs){
        ops.push_back(id_allocator.allocate(i));
    }
    op_queue.alloc(op_valid_num, ops);
    auto MOP_wave = op_queue.issue(idvol.level_sum, idvol);
    mop_queue.alloc(MOP_wave.first, MOP_wave.second);
    auto UOP_wave = mop_queue.issue(idvol.line_sum, idvol);
    uop_queue.alloc(UOP_wave.first, UOP_wave.second);
    std::vector<CanonicalEvent> events = uop_queue.exec_n_commit(idvol);
    auto end_events = EndEvents(events);
    auto new_retire_events = NewRetireEvents(events);
    auto [trade_events, tick_events, reply_events] = router_u.RouteEvents(events, id_allocator);
    op_queue.release(end_events, new_retire_events);
    id_allocator.release(new_retire_events);
    return {trade_events, tick_events, reply_events}; // EndEvents and NewRetireEvents is internal
}
