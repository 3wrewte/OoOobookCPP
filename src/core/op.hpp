#pragma once
#include <array>
#include <unordered_map>

#include "types.hpp"
#include "addr.hpp"
#include "wire.hpp"
#include "uop.hpp"
#include "mop.hpp"
#include "registers.hpp"
#include "instructions.hpp"

constexpr size_t   OP_ROB_LENGTH = 16;
constexpr uint64_t OP_ROB_MASK   = OP_ROB_LENGTH - 1;
constexpr size_t   OP_ROB_BITS = 4;
struct OPQueue{ // a  single-tick substitution of op ROB
    std::array<OP, OP_ROB_LENGTH> op_queue; // register!!!, but write-immediate,read-through
    std::array<OPcontext, OP_ROB_LENGTH> opcontext_queue;
    wire<bool> alloc_lock;
    Register<uint32_t> head;
    Register<uint32_t> issued_tail; // = non-issued head
    Register<uint32_t> tail;
    std::unordered_map<OrderID_t, CanonicalAddr_t> resting_index; // already contain price
    //Register<OrderID_t> count;

    size_t used() const { return (tail.get() - head.get()) & OP_ROB_MASK; }
    size_t ready_num() const { return (OP_ROB_LENGTH - 1) - used(); }

    void reset();
    void prepare();
    void commit();

    void alloc(size_t valid_num, std::vector<OP> &ops, std::vector<OPcontext> &opcs);


    std::pair<size_t, std::vector<Mop> >  issue(LevelSumMap & VirtualLevelSum, IdVolMap & idvol); // will change to Line SumCache

    std::vector<std::pair<OP,OPcontext> > release (std::vector<CanonicalEvent> &events);//listen to Canonical Event
};

inline void OPQueue::alloc(size_t valid_num, std::vector<OP>& ops, std::vector<OPcontext> &opcs)
{
    assert(valid_num <= ops.size());
    assert(valid_num <= opcs.size());
    const size_t num = std::min(valid_num, ready_num()); // handshake
    alloc_lock.set(true);//prevent multi drive
    for(size_t i = 0; i < num; i++)op_queue       [(tail.get() + i) & OP_ROB_MASK] = ops[i];
    for(size_t i = 0; i < num; i++)opcontext_queue[(tail.get() + i) & OP_ROB_MASK] = opcs[i];
    tail.set((tail.get() + num) & OP_ROB_MASK);
}

inline std::pair<size_t, std::vector<Mop> > OPQueue::issue(LevelSumMap & VirtualLevelSum, IdVolMap & idvol)
{
    std::vector <Mop> results;
    if(issued_tail.get() != tail.get()){  // only issue one OP for now
        OP op = op_queue[issued_tail.get()];
        OpSeq op_seq = issued_tail.get();
        issued_tail.set((issued_tail.get() + 1) & ((1 << OP_ROB_BITS) - 1)); // issue 1 op
        if(op.kind == OPKind::Limit || op.kind == OPKind::Market){
            const Price_t best = (op.side == Side_t::Buy)? idvol.meta.best_ask().value_or(Price_max)
                : idvol.meta.best_bid().value_or(0);
            const Price_t limit = (op.kind == OPKind::Limit)? op.imm0
                : (op.side == Side_t::Buy)? Price_max : 0;
            const size_t active = idvol.meta.sides[!op.side].active_levels;

            const Qty_t &total_budget = op.imm1;
            Qty_t remaining_budget = total_budget;

            Price_t current_price = best;
            size_t remaining_active = active;
            while(((op.side == Side_t::Sell && current_price >= limit && remaining_active)
                || (op.side == Side_t::Buy  && current_price <= limit && remaining_active)))
            {
                const CanonicalAddr_t LevelSum_addr = CanonicalAddrPack(CanonicalAddrStructured{
                    .sym   = op.sym       ,
                    .side  = (op.side == Side_t::Buy)? Side_t::Sell : Side_t::Buy, // opposite
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
                        .op_seq = op_seq,
                        .op_end = (!remaining_budget),
                        .addr = CanonicalAddrPack(CanonicalAddrStructured{
                            .sym = op.sym,
                            .side = (op.side == Side_t::Buy)? Side_t::Sell : Side_t::Buy, // opposite
                            .price = current_price,
                            .pos = 0
                        }),
                        .imm = traded_vol
                    });
                }

                if(traded_vol && traded_vol == current_levelsum)remaining_active --;
                if(!remaining_budget || !remaining_active)break;
                current_price = (op.side == Side_t::Buy)?
                    idvol.meta.next_ask_after(current_price, limit).value_or(Price_max)
                :   idvol.meta.next_bid_below(current_price, limit).value_or(0);
            }

            if(remaining_budget && op.kind == OPKind::Limit){
                results.push_back(Mop{
                    .kind = MopKind::InsertLevel,
                    .id = op.id,
                    .op_seq = op_seq,
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
            if(results.empty()){ // none mOP has issued, then put an end mop
                    results.push_back(Mop{
                        .kind = MopKind::End,
                        .id = op.id,
                        .op_seq = op_seq,
                        .op_end = true,
                        .addr = 0,
                        .imm = 0
                    });
            } else {
                results.back().op_end = true;
            }

        }
        else if(op.kind == OPKind::Cancel){
            // in multi-tick model, and resting_index may delete elements: if resting_index does not contain target, search for op rob from head to present position, if found, then wait until it returns, if not found, then it means target invalid or already fully consumed, send end mop
            if(resting_index.find(op.imm0) != resting_index.end()){ // since uOP, mOP ROB is always empty now, resting_index should always get value
                const CanonicalAddr_t addr = resting_index[op.imm0];
                results.push_back(Mop{
                    .kind = MopKind::CancelLevel,
                    .id = op.id,
                    .op_seq = op_seq,
                    .op_end = true,
                    .addr = addr,
                    .imm = op.imm0
                });
            }
            else{
                results.push_back(Mop{
                    .kind = MopKind::End,
                    .id = op.id,
                    .op_seq = op_seq,
                    .op_end = true,
                    .addr = 0,
                    .imm = 0
                });
            }
        }
    }
    else issued_tail.set(issued_tail.get()); // nothing issue
    return std::make_pair(results.size(), results);
}

inline std::vector<std::pair<OP,OPcontext> > OPQueue::release (std::vector<CanonicalEvent> &events){
    std::vector<std::pair<OP,OPcontext> > res;
    size_t commit_front = head.get();
    for(auto &i : events){
        if(i.op_end){
            assert(i.id == op_queue[commit_front].id);
            assert(i.op_seq == commit_front);
            res.push_back(std::make_pair(op_queue[commit_front], opcontext_queue[commit_front]));
            commit_front = (commit_front + 1) & ((1 << OP_ROB_BITS) - 1);
        }
        if(i.kind == UopKind::InsertSlot){
            resting_index[i.id] = std::get<CanonicalAddr_t>(i.payload); // input only now
        }
    }
    head.set(commit_front);
    return res;
}

inline void OPQueue::reset()
{
    op_queue = {};
    opcontext_queue = {};
    alloc_lock.reset();
    head.reset();
    tail.reset();
    issued_tail.reset();
    //count.reset();
    resting_index = {};
}

inline void OPQueue::prepare()
{
    alloc_lock.prepare();
    head.prepare();
    tail.prepare();
    issued_tail.prepare();
    ///count.prepare();
}

inline void OPQueue::commit()
{
    alloc_lock.commit();
    head.commit();
    tail.commit();
    issued_tail.commit();
    //count.commit();
}

