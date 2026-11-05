#pragma once
#include <queue>

#include "types.hpp"
#include "addr.hpp"
#include "wire.hpp"
#include "uop.hpp"
#include "mop.hpp"
#include "registers.hpp"
enum class OPKind : uint8_t { Limit = 0b00, Market = 0b01, Cancel = 0b10};

struct OP {
    OrderID_t       id        ;
    OPKind          kind      ;
    Sym_t           sym       ;
    Side_t          side      ;
    uint32_t        imm0      ; // Limit: price, market: empty, Cancel: target_id
    uint32_t        imm1      ; // Limit/market: qty, Cancel: empty
};


struct OPQueue{ // a  single-tick substitution of op ROB
    std::queue<OP> op_queue; // register!!!, but write-immediate, read-through
    size_t ready_num;
    wire<bool> alloc_lock;
    Register<uint32_t> head; // substitution of head register, used to calculate op_seq

    void reset();
    void prepare();
    void commit();

    void alloc(size_t num, std::vector<OP> &ops);


    std::pair<size_t, std::vector<Mop> >  issue(LevelSumMap & VirtualLevelSum, IdVolMap & idvol); // will change to Line SumCache
};

inline void OPQueue::alloc(size_t num, std::vector<OP>& ops)
{
    assert(num <= ops.size());
    alloc_lock.set(true);//prevent multi drive
    for(size_t i = 0; i < num; i++)op_queue.push(ops[i]);
    head.set(head.get() + num);
}

inline std::pair<size_t, std::vector<Mop> > OPQueue::issue(LevelSumMap & VirtualLevelSum, IdVolMap & idvol)
{
    std::vector <Mop> results;
    if(!op_queue.empty()){  // only issue one OP for now
        OP op = op_queue.front();
        op_queue.pop();
        if(op.kind == OPKind::Limit || op.kind == OPKind::Market){
            const Price_t best = (op.side == Side_t::Buy)? idvol.meta.best_bid().value_or(0)
                : idvol.meta.best_ask().value_or(Price_max);
            const Price_t limit = (op.kind == OPKind::Limit)? op.imm0
                : (op.side == Side_t::Buy)? 0 : Price_max;
            const size_t active = idvol.meta.sides[op.side].active_levels;

            const Qty_t &total_budget = op.imm1;
            Qty_t remaining_budget = total_budget;

            Price_t current_price = best;
            size_t remaining_active = active;
            while(((op.side == Side_t::Buy  && current_price >= limit && remaining_active)
                || (op.side == Side_t::Sell && current_price <= limit && remaining_active)))
            {
                const CanonicalAddr_t LevelSum_addr = CanonicalAddrPack(CanonicalAddrStructured{
                    .sym   = op.sym       ,
                    .side  = op.side      ,
                    .price = current_price,
                    .pos   = 0
                });
                const QtySum_t current_levelsum = VirtualLevelSum.read_single_through(LevelSumCacheIndexing(CanonicalAddrUnpack(LevelSum_addr)));
                const Qty_t traded_vol = std::min(QtySum_t(remaining_budget), current_levelsum);
                remaining_budget -= traded_vol;
                if(traded_vol){
                    results.push_back(Mop{
                        .kind = MopKind::TakeLevel,
                        .id = op.id,
                        .op_seq = 0, // should be the index in ROB, but since we only issue one, then use 0
                        .op_end = (!remaining_budget),
                        .addr = CanonicalAddrPack(CanonicalAddrStructured{
                            .sym = op.sym,
                            .side = op.side,
                            .price = current_price,
                            .pos = 0
                        }),
                        .imm = traded_vol
                    });
                }

                if(traded_vol && traded_vol == current_price)remaining_active --;
                if(!remaining_budget)break;
                current_price = (op.side == Side_t::Buy)?
                    idvol.meta.next_bid_below(current_price, limit).value_or(0)
                :   idvol.meta.next_ask_after(current_price, limit).value_or(Price_max);
            }

            if(remaining_budget && op.kind == OPKind::Limit){
                results.push_back(Mop{
                    .kind = MopKind::InsertLevel,
                    .id = op.id,
                    .op_seq = 0, // should be the index in ROB, but since we only issue one, then use 0
                    .op_end = true,
                    .addr = CanonicalAddrPack(CanonicalAddrStructured{
                        .sym = op.sym,
                        .side = op.side,
                        .price = limit,
                        .pos = 0
                    }),
                    .imm = remaining_budget
                });
            }
            else if(remaining_budget && op.kind == OPKind::Market){
                //warning: cannot find enough opposite volume
            }

        }
        else if(op.kind == OPKind::Cancel){

        }
    }
    return std::make_pair(results.size(), results);
}



inline void OPQueue::reset()
{
    op_queue = {};
    ready_num = std::numeric_limits<size_t>::max();
    alloc_lock.reset();
    head.reset();
}

inline void OPQueue::prepare()
{
    alloc_lock.prepare();
    head.prepare();
}

inline void OPQueue::commit()
{
    ready_num = std::numeric_limits<size_t>::max();
    alloc_lock.commit();
    head.commit();
}

