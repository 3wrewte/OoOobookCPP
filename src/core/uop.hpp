#pragma once

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <queue>
#include <vector>
#include "types.hpp"
#include "addr.hpp"
#include "idvol.hpp"
#include "wire.hpp"

struct TakeLineResult{
    IDLine   trade_id_vec    ;
    VolLine  trade_vol_vec   ;
    Qty_t    consumed        ;
    //Qty_t    remaining_budget; // should always be zero
    QtySum_t line_remaining  ; // needed or not?
};

inline TakeLineResult TakeLineCalc(const Line & line, const Qty_t& budget){
    Qty_t remaining_budget = budget;
    QtySum_t remaining_volume = 0;
    VolLine vol_vec;
    IDLine  id_vec ;
    for(size_t i = 0; i < 16; i++){
        if(remaining_budget && line.Vols[i]){
            id_vec[i] = line.IDs[i];
            vol_vec[i] = std::min(line.Vols[i], remaining_budget);
            remaining_budget -= vol_vec[i];
        }
        remaining_volume += line.Vols[i] - vol_vec[i];
    }
    return TakeLineResult{
        .trade_id_vec = id_vec,
        .trade_vol_vec = vol_vec,
        .consumed = budget - remaining_budget,
        .line_remaining = remaining_volume
    };
}

// enum uOPStatus : uint8_t {
//     non_valid    = 0b0000,
//     allocated    = 0b0001,
//     issued       = 0b0011,
//     written_back = 0b0111,
//     submitted    = 0b0000
// };

enum class UopKind { TakeLine, InsertSlot, CancelSlot};

struct CanonicalEvent {
    UopKind kind;
    OrderID_t id;
    OpSeq op_seq;
    //MopSeq parent_mop;
    //UopSeq uop_seq;
    bool op_end;
    /* payload: Take(traded_ids,take_vols,consumed) | Insert | Cancel | End */
};

struct TakeCanonicalEvent : CanonicalEvent{
    TakeLineResult result;
};
struct CancelCanonicalEvent : CanonicalEvent{
    CancelResult result;
};


struct Uop {
    UopKind kind;
    OrderID_t       id        ;
    // MopSeq          parent_mop;
    OpSeq           op_seq    ;
    bool            op_end    ;
    CanonicalAddr_t addr      ; // Take: addr = line_key << line_bits, Insert/Cancel: addr
    uint32_t        imm       ; // Take: budget, Insert: volume, Cancel: target_id
};
/*
struct TakeUop : Uop{
    IDVol_key_t lineaddr;
    Qty_t budget;
};

struct InsertUop : Uop{
    CanonicalAddr_t addr;
    Qty_t volume;
};

struct CancelUop : Uop{
    CanonicalAddr_t addr;
    OrderID_t target_id;
};*/


struct UopQueue{ // substitution of Uop ROB, hence using a different name, since single-clock,
    // std::queue<  TakeUop>   TakeQueue;//since single-clock, these queues are wire, not registers, and hence do not need head, // tail, size
    // std::queue<InsertUop> InsertQueue;
    // std::queue<CancelUop> CancelQueue;
    // size_t   Take_ready_num; // register, but always maximum here, should refer to the remaining capacity of ROB
    // size_t Insert_ready_num;
    // size_t Cancel_ready_num;
    // wire<bool>   take_alloc_lock; // ensure only alloc once
    // wire<bool> insert_alloc_lock;
    // wire<bool> cancel_alloc_lock;
    std::queue<Uop>  uop_queue;
    size_t ready_num;
    wire<bool> alloc_lock;

    void reset();
    void prepare();
    void commit();

    // void alloc_take  (size_t num, std::vector<TakeUop>   &uops); // should be num, std::array<TakeUop, max alloc num> in  multi-tick version
    // void alloc_insert(size_t num, std::vector<InsertUop> &uops);
    // void alloc_cancel(size_t num, std::vector<CancelUop> &uops);
    void alloc(size_t num, std::vector<Uop> &uops);

    std::vector<CanonicalEvent> exec_n_commit(IdVolMap &idvol);
    /*v1 scheme:
     * void exec();
     * std::vector<CanonicalEvent> commit()// if multi-tick model, should be std::array
     */

};

inline void UopQueue::alloc(size_t num, std::vector<Uop>& uops)
{
    assert(num <= uops.size());
    alloc_lock.set(true);//prevent multi drive
    for(size_t i = 0; i < num; i++)uop_queue.push(uops[i]);
}

inline std::vector<CanonicalEvent> UopQueue::exec_n_commit(IdVolMap &idvol)
{
    std::vector<CanonicalEvent> results;
    //first, exec insert since there will not be any hazard
    while(!uop_queue.empty()){
        Uop uop = uop_queue.front();
        uop_queue.pop();
        if(uop.kind == UopKind::TakeLine){
            //exec
            //CanonicalAddrStructured addr_structured =  CanonicalAddrUnpack(uop.addr);
            Line current_line = idvol.read_line(uop.addr);
            TakeLineResult take_result = TakeLineCalc(current_line, uop.imm); // uop stores this data
            // exec end

            // commit
            idvol.take_line(uop.addr, take_result.trade_vol_vec, take_result.consumed);
            TakeCanonicalEvent res = TakeCanonicalEvent{.result = take_result};
            res.kind = uop.kind;
            res.id = uop.id;
            res.op_seq = uop.op_seq;
            res.op_end = uop.op_end;
            results.push_back(res);
            //commit end
        }else if(uop.kind == UopKind::InsertSlot){
            // non exec

            // commit
            idvol.insert_slot(uop.addr, uop.id, uop.imm);
            results.push_back(CanonicalEvent{
                .kind = uop.kind,
                .id = uop.id,
                .op_seq = uop.op_seq,
                .op_end = uop.op_end
            });
            //commit end
        }else if(uop.kind == UopKind::CancelSlot){
            // non exec

            // commit
            CancelResult cancel_result = idvol.cancel_slot(uop.addr, uop.imm);
            CancelCanonicalEvent res = CancelCanonicalEvent{.result = cancel_result};
            res.kind = uop.kind;
            res.id = uop.id;
            res.op_seq = uop.op_seq;
            res.op_end = uop.op_end;
            results.push_back(res);
            // commit end
        }
    }
    return results;
}



inline void UopQueue::reset()
{
    uop_queue = {};
    ready_num = ~(0u);
    alloc_lock.reset();
}

inline void UopQueue::prepare()
{
    uop_queue = {};
    alloc_lock.prepare();
}

inline void UopQueue::commit()
{
    ready_num = ~(0u);
    alloc_lock.commit();
}
