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

struct core_top{
    IdVolMap idvol;
    OPQueue   op_queue;
    MopQueue mop_queue;
    UopQueue uop_queue;

    void reset  ();
    void prepare();
    void commit ();

    std::pair<size_t, std::vector<CanonicalEvent> >  //output: ready_num, events
        comb(size_t valid_num, std::vector<OP>& OPs); // input: valid_num, OPs
};

void core_top::reset  (){
    idvol    .reset();
     op_queue.reset();
    mop_queue.reset();
    uop_queue.reset();
}
void core_top::prepare(){
    //idvol; write-immediate, hence no prepare yet
     op_queue.prepare();
    mop_queue.prepare();
    uop_queue.prepare();
}
void core_top::commit (){
    //idvol; write-immediate, hence no commit yet
     op_queue.commit();
    mop_queue.commit();
    uop_queue.commit();
}

std::pair<size_t, std::vector<CanonicalEvent> >
core_top::comb(size_t op_valid_num, std::vector<OP>& OPs){
    op_queue.alloc(op_valid_num, OPs);
    auto MOP_wave = op_queue.issue(idvol.level_sum, idvol);
    mop_queue.alloc(MOP_wave.first, MOP_wave.second);
    auto UOP_wave = mop_queue.issue(idvol.line_sum, idvol);
    uop_queue.alloc(UOP_wave.first, UOP_wave.second);
    std::vector<CanonicalEvent> events = uop_queue.exec_n_commit(idvol);
    op_queue.release(events);
    return std::make_pair(events.size(), events);
}
