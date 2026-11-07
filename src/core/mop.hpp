#pragma once
#include <queue>

#include "types.hpp"
#include "addr.hpp"
#include "wire.hpp"
#include "uop.hpp"
#include "instructions.hpp"


struct MopQueue{ // a  single-tick substitution of Mop ROB
    std::queue<Mop> mop_queue;
    size_t ready_num;
    wire<bool> alloc_lock;

    void reset();
    void prepare();
    void commit();

    void alloc(size_t num, std::vector<Mop> &mops);


    std::pair<size_t, std::vector<Uop> >  issue(LineSumMap & VirtualLineSum, IdVolMap & idvol); // will change to Line SumCache
};

inline void MopQueue::alloc(size_t num, std::vector<Mop>& mops)
{
    assert(num <= mops.size());
    alloc_lock.set(true);//prevent multi drive
    for(size_t i = 0; i < num; i++)mop_queue.push(mops[i]);
}

inline std::pair<size_t, std::vector<Uop> > MopQueue::issue(LineSumMap & VirtualLineSum, IdVolMap & idvol)
{
    std::vector <Uop> results;
    while(!mop_queue.empty()){
        Mop mop = mop_queue.front();
        mop_queue.pop();
        if(mop.kind == MopKind::TakeLevel){

            const LevelAddrStructured moplevel = LevelAddrUnpack(Canonical_to_LevelAddr(mop.addr));
            const Pos_t head_line = idvol.meta_head_line(Canonical_to_LevelAddr(mop.addr)).value_or(0);
            const Pos_t tail = idvol.tail_ram(mop.addr) & ((1 << Pos_bits) - 1);
            const Pos_t tail_line = tail >> 4;

            const Qty_t &total_budget = mop.imm;
            Qty_t remaining_budget = total_budget;

            for(Pos_t i = head_line >> 4; i <= tail_line >> 4; i++){    /// i = pos[27:8]
                const CanonicalAddr_t LineSum_line_addr = CanonicalAddrPack(CanonicalAddrStructured{
                    .sym   = moplevel.sym,
                    .side  = moplevel.side,
                    .price = moplevel.price,
                    .pos = i << 8
                });
                std::array<QtySum_t, 16> LineSum_line = VirtualLineSum.read_row_through(LineSumCacheIndexing(CanonicalAddrUnpack(LineSum_line_addr)));
                for(Pos_t j = 0; j < 16; j++){ // j = pos[7:4]
                    const Qty_t traded_vol = std::min(QtySum_t(remaining_budget), LineSum_line[j]);
                    remaining_budget -= traded_vol;
                    if(traded_vol){
                        const CanonicalAddr_t uop_addr = CanonicalAddrPack(CanonicalAddrStructured{
                            .sym   = moplevel.sym,
                            .side  = moplevel.side,
                            .price = moplevel.price,
                            .pos = (i << 8) | (j << 4)
                        });
                        results.push_back(Uop{
                            .kind = UopKind::TakeLine,
                            .id   = mop.id,
                            .op_seq = mop.op_seq,
                            .op_end = mop.op_end && (!remaining_budget),
                            .addr   = uop_addr,
                            .imm    = traded_vol
                        });
                    }
                    if(!remaining_budget)break;
                }
                if(!remaining_budget)break;
            }
            //if(remaining_budget){} // we can deal with this situation but it should not happen
            assert(remaining_budget == 0);
        }
        else if(mop.kind == MopKind::InsertLevel){
            const Pos_t tail = idvol.tail_ram(mop.addr) & ((1 << Pos_bits) - 1);
            const LevelAddrStructured moplevel = LevelAddrUnpack(Canonical_to_LevelAddr(mop.addr));
            results.push_back(Uop{
                .kind = UopKind::InsertSlot,
                .id   = mop.id,
                .op_seq = mop.op_seq,
                .op_end = mop.op_end,
                .addr   = CanonicalAddrPack(CanonicalAddrStructured{
                    .sym = moplevel.sym,
                    .side = moplevel.side,
                    .price = moplevel.price,
                    .pos = tail
                }),
                .imm    = mop.imm
            });
        }
        else if(mop.kind == MopKind::CancelLevel){
            results.push_back(Uop{
                .kind = UopKind::CancelSlot,
                .id   = mop.id,
                .op_seq = mop.op_seq,
                .op_end = mop.op_end,
                .addr   = mop.addr,
                .imm    = mop.imm
            });
        }
        else if(mop.kind == MopKind::End){
            results.push_back(Uop{
                .kind = UopKind::End,
                .id   = mop.id,
                .op_seq = mop.op_seq,
                .op_end = mop.op_end,
                .addr   = mop.addr,
                .imm    = mop.imm
            });
        }
    }
    return std::make_pair(results.size(), results);
}



inline void MopQueue::reset()
{
    mop_queue = {};
    ready_num = std::numeric_limits<size_t>::max();
    alloc_lock.reset();
}

inline void MopQueue::prepare()
{
    mop_queue = {};
    alloc_lock.prepare();
}

inline void MopQueue::commit()
{
    ready_num = std::numeric_limits<size_t>::max();
    alloc_lock.commit();
}
