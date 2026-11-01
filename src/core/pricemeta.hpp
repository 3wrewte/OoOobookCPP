#pragma once
#include<cstdint>
#include<cstddef>
#include<optional>
#include<unordered_map>
#include<array>
#include<cassert>
#include<algorithm>
#include"types.hpp"
#include"phase.hpp"
#include "addr.hpp"

struct PriceMeta {
    QtySum_t level_sum ;
    Pos_t    head_line ;//pos[27:4], Hint, NOT AUTHORIZED !!!
    bool     head_valid;
    Pos_t    tail_pos  ;
    bool     active    ;
};
struct SideMeta{
    std::unordered_map<Price_t, PriceMeta> PriceMetaRAM;
    std::unordered_map<Price_t, uint16_t > row_masks   ; // mask price[3:0] per row
    std::unordered_map<Price_t, uint64_t > block_masks ; // mask price[9:4] per block
    std::array        <uint64_t,4096     > super_masks ; // mask price[15:10] per superblock, index price[27:16] in total 4096
    size_t active_levels;
    std::optional<Price_t> best;

    void reset(){
        PriceMetaRAM.clear();
        row_masks   .clear();
        block_masks .clear();
        super_masks .fill(0);
        active_levels = 0;
        best = std::nullopt;
    }

    void set_active(Price_t price, bool active){
        const Price_t row_index   = price >> 4 ;
        const Price_t block_index = price >> 10;
        const Price_t super_index = price >> 16;
        const Price_t row_offset  = price & 0x0f; // price[3:0]
        const Price_t block_offset= (price >> 4 ) & 0x3f; // price[9:4]
        const Price_t super_offset= (price >> 10) & 0x3f; // price[15:10]
        if (active){ //activate
            if(!(row_masks[row_index] & (1ull << row_offset))) active_levels ++;// some level flips from non-active to active
            row_masks[row_index]     |= 1ull << row_offset  ;

            block_masks[block_index] |= 1ull << block_offset;

            super_masks[super_index] |= 1ull << super_offset;
        }
        else{ // deactivate
            bool   row_empty = true;
            bool block_empty = true;
            if(row_masks.find(row_index) != row_masks.end()){ //if not find, then do nothing
                if (row_masks[row_index] & (1ull << row_offset)) active_levels--; // some level flips from active to non-active
                row_masks[row_index] &= ~(1ull << row_offset);
                if (row_masks[row_index] == 0)row_masks.erase(row_index);
                else row_empty = false;
            }

            if(block_masks.find(block_index) != block_masks.end()){ //if not find, then do nothing
                if(row_empty){
                    block_masks[block_index] &= ~(1ull << block_offset);
                    if (block_masks[block_index] == 0)block_masks.erase(block_index);
                    else block_empty = false;
                }
                else block_empty = false; // row not empty, block must not be empty
            }

            if(block_empty)
            super_masks[super_index] &= ~(1ull << super_offset);
        }

    }

    void refresh_best_after_remove(Side_t side, Price_t price){
        if(best.has_value()){
            if(row_masks.find(best.value() >> 4) != row_masks.end()){
                if (row_masks[best.value() >> 4] & (1ull << (best.value() & 0x0f))){ // best is valid and still active
                    return;
                }
            }
        }
        // search for best

        if(side == Side_t::Buy){
            best = search_lower_active(price, Price_min);
        }
        else if(side == Side_t::Sell){
            best = search_upper_active(price, Price_max);// notice, if Price_bit > 32, should be ~(0x0ull)
        }
    }

    void refresh_best_after_insert(Side_t side, Price_t price){
        if(side == Side_t::Buy){
            best = std::max(price, best.value_or(Price_min));
        }
        else if(side == Side_t::Sell){
            best = std::min(price, best.value_or(Price_max));
        }
    }

    std::optional<Price_t> search_upper_active(Price_t start, Price_t end){
        assert(start <= end);
        assert(start <= Price_max);
        assert(  end <= Price_max);
        //Price_t start_row_key   = start >> 4 ;
        const Price_t start_block_key = start >> 10;
        const Price_t start_super_key = start >> 16;
        const Price_t start_row_offset  = start & 0x0f; // price[3:0]
        const Price_t start_block_offset= (start >> 4 ) & 0x3f; // price[9:4]
        const Price_t start_super_offset= (start >> 10) & 0x3f; // price[15:10]

        //Price_t end_row_key   =    end >> 4 ;
        const Price_t end_block_key =    end >> 10;
        const Price_t end_super_key =    end >> 16;
        const Price_t end_row_offset  =  end & 0x0f; // price[3:0]
        const Price_t end_block_offset= (end >> 4 ) & 0x3f; // price[9:4]
        const Price_t end_super_offset= (end >> 10) & 0x3f; // price[15:10]

        Price_t   lower_row_offset; //= start & 0x0f; // price[3:0]
        Price_t lower_block_offset; //= (start >> 4 ) & 0x3f; // price[9:4]

        Price_t   upper_row_offset; //= end & 0x0f; // price[3:0]
        Price_t upper_block_offset; //= (end >> 4 ) & 0x3f; // price[9:4]

        Price_t   res_row_key ;
        Price_t res_block_key ;
        Price_t res_super_key ;
        Price_t   res_row_offset; // price[3:0]
        Price_t res_block_offset; // price[9:4]
        Price_t res_super_offset; // price[15:10]

        for(int64_t i = start_super_key; i <= end_super_key; i++ ){ // i is signed
            uint64_t current_mask = super_masks[i];
            if(i == start_super_key) current_mask &= upper_one_mask(start_super_offset);
            if(i ==   end_super_key)current_mask &= lower_one_mask(  end_super_offset);
            if(current_mask){
                res_super_key    = i;
                res_super_offset = LSB(current_mask).value_or(0);
                lower_block_offset = (start_super_key == res_super_key && start_super_offset == res_super_offset)? start_block_offset : 0x00u;
                upper_block_offset = (  end_super_key == res_super_key && end_super_offset == res_super_offset)?                end_block_offset // if start and end is in same super block, search until end block offset
                    : 0x3fu; //if start and end not in same super block, serch untill the end of block
                goto super_done;
            }
        }
        return std::nullopt;
    super_done:
        res_block_key = (res_super_key << 6) | res_super_offset;// block key = {super key(12bit), offset(6bit)}
        const uint64_t current_block = block_masks[res_block_key] //we already confirm that this block non-empty, and hence will not insert zero line
            & upper_one_mask(lower_block_offset)
            & lower_one_mask(upper_block_offset);
        if(!current_block)return std::nullopt;
        res_block_offset = LSB(current_block).value_or(0);
        res_row_key = (res_block_key << 6)
            | res_block_offset;
        lower_row_offset = (res_block_key == start_block_key && res_block_offset == start_block_offset)? start_row_offset
            : 0x00u;
        upper_row_offset = (res_block_key == end_block_key && res_block_offset == end_block_offset)? end_row_offset
            : 0x3fu;


        const uint64_t current_row = row_masks[res_row_key]
            & upper_one_mask(lower_row_offset)
            & lower_one_mask(upper_row_offset);
        if(!current_row)return std::nullopt;
        res_row_offset = LSB(current_row).value_or(0);

        return (res_row_key) << 4 | res_row_offset;
    }
    std::optional<Price_t> search_lower_active(Price_t start, Price_t end){
        assert(start >= end);
        assert(start <= Price_max);
        assert(  end <= Price_max);
        //Price_t start_row_key   = start >> 4 ;
        const Price_t start_block_key = start >> 10;
        const Price_t start_super_key = start >> 16;
        const Price_t start_row_offset  = start & 0x0f; // price[3:0]
        const Price_t start_block_offset= (start >> 4 ) & 0x3f; // price[9:4]
        const Price_t start_super_offset= (start >> 10) & 0x3f; // price[15:10]

        //Price_t end_row_key   =    end >> 4 ;
        const Price_t end_block_key =    end >> 10;
        const Price_t end_super_key =    end >> 16;
        const Price_t end_row_offset  =  end & 0x0f; // price[3:0]
        const Price_t end_block_offset= (end >> 4 ) & 0x3f; // price[9:4]
        const Price_t end_super_offset= (end >> 10) & 0x3f; // price[15:10]

        Price_t   lower_row_offset; //= start & 0x0f; // price[3:0]
        Price_t lower_block_offset; //= (start >> 4 ) & 0x3f; // price[9:4]

        Price_t   upper_row_offset; //= end & 0x0f; // price[3:0]
        Price_t upper_block_offset; //= (end >> 4 ) & 0x3f; // price[9:4]

        Price_t   res_row_key ;
        Price_t res_block_key ;
        Price_t res_super_key ;
        Price_t   res_row_offset; // price[3:0]
        Price_t res_block_offset; // price[9:4]
        Price_t res_super_offset; // price[15:10]

        for(int64_t i = start_super_key; i >= end_super_key; i-- ){ // i must be signed, otherwise will overflow at 0
            uint64_t current_mask = super_masks[i];
            if(i == start_super_key) current_mask &= lower_one_mask(start_super_offset);
            if(i ==   end_super_key)current_mask &= upper_one_mask(  end_super_offset);
            if(current_mask){
                res_super_key    = i;
                res_super_offset = MSB(current_mask).value_or(0);
                lower_block_offset = (  end_super_key == res_super_key &&   end_super_offset == res_super_offset)? end_block_offset : 0x00u;
                upper_block_offset = (start_super_key == res_super_key && start_super_offset == res_super_offset)?                start_block_offset // if start and end is in same super block, search until end block offset
                : 0x3fu; //if start and end not in same super block, serch untill the end of block
                goto super_done;
            }
        }
        return std::nullopt;
        super_done:
        res_block_key = (res_super_key << 6) | res_super_offset;// block key = {super key(12bit), offset(6bit)}
        const uint64_t current_block = block_masks[res_block_key]
        & upper_one_mask(lower_block_offset)
        & lower_one_mask(upper_block_offset);
        if(!current_block)return std::nullopt;
        res_block_offset = MSB(current_block).value_or(0);
        res_row_key = (res_block_key << 6)
        | res_block_offset;
        upper_row_offset = (res_block_key == start_block_key && res_block_offset == start_block_offset)? start_row_offset
        : 0x3fu;
        lower_row_offset = (res_block_key == end_block_key && res_block_offset == end_block_offset)? end_row_offset
        : 0x00u;


        const uint64_t current_row = row_masks[res_row_key]
        & upper_one_mask(lower_row_offset)
        & lower_one_mask(upper_row_offset);
        if(!current_row)return std::nullopt;
        res_row_offset = MSB(current_row).value_or(0);

        return (res_row_key) << 4 | res_row_offset;
    }
};

struct BookMeta { // temporary, not RTL

    uint64_t version;
    SideMeta sides[2];

    void reset(){
        version = 0;
        sides[Side_t::Buy ].reset();
        sides[Side_t::Sell].reset();
    }
    void sync_level_after_write(Side_t side, Price_t price, QtySum_t old_sum, QtySum_t new_sum, Pos_t touched_line, QtySum_t touched_line_sum, Pos_t tail_pos);
    std::optional<Price_t> best_ask(){return sides[Side_t::Sell].best;}       // 最低卖价
    std::optional<Price_t> best_bid(){return sides[Side_t::Buy ].best;}       // 最高买价
    std::optional<Price_t> next_ask_after(Price_t p, Price_t limit) // 从 p + 1 向上走下一个活跃卖价，止于 limit
    {
        if(p + 1 > limit) return std::nullopt;
        return sides[Side_t::Sell].search_upper_active(p + 1, limit);
    }
    std::optional<Price_t> next_bid_below(Price_t p, Price_t limit)// 从 p - 1 向下走
    {
        if(p <= limit) return std::nullopt;
        return sides[Side_t::Buy ].search_lower_active(p - 1, limit);
    }

    uint64_t get_version(){return version;}

    PriceMeta get_price_meta(Side_t side, Price_t price){
        if(sides[side].PriceMetaRAM.find(price) == sides[side].PriceMetaRAM.end())return PriceMeta();
        return sides[side].PriceMetaRAM[price];
    }

    std::optional<Pos_t> get_meta_head_line(Side_t side, Price_t price){
        if(sides[side].PriceMetaRAM.find(price) == sides[side].PriceMetaRAM.end())return std::nullopt;
        if(sides[side].PriceMetaRAM[price].head_valid)return sides[side].PriceMetaRAM[price].head_line;
        else return std::nullopt;

    }

    void  set_meta_head_line(Side_t side, Price_t price, Pos_t head_line){
        PriceMeta p = sides[side].PriceMetaRAM[price];
        p.head_line = head_line;
        p.head_valid = true;
        sides[side].PriceMetaRAM[price] = p;
        return;
    }
};

inline void BookMeta::sync_level_after_write(Side_t side, Price_t price, QtySum_t old_sum, QtySum_t new_sum, Pos_t touched_line, QtySum_t touched_line_sum, Pos_t tail_pos)
{
    if(sides[side].PriceMetaRAM.find(price) == sides[side].PriceMetaRAM.end()){
        sides[side].PriceMetaRAM[price] = PriceMeta();
    }
    PriceMeta &p = sides[side].PriceMetaRAM[price];

    bool was_active = p.active;
    bool is_active  = (new_sum != 0);

    // Modify PriceMeta
    if(!was_active && !is_active){
        p.active = false;
        p.head_valid = false;
    }
    else if (!was_active &&  is_active){
        p.active = true;
        p.head_valid = true;
        p.head_line = touched_line; // this modification activates the level

    }
    else if ( was_active && !is_active){
        p.active = false;
        p.head_valid = false;
        p.head_line = touched_line; // at this time head = tail-1 = last modified position
    }
    else if ( was_active &&  is_active){
        if (p.head_valid && p.head_line == touched_line && touched_line_sum == 0 ){
            p.head_valid = false;
        }
    }

    //if active changes, update masks and version
    if( was_active && !is_active){
        sides[side].set_active(price, false);
        sides[side].refresh_best_after_remove(side, price);
        version++;
    }
    if(!was_active && is_active){
        sides[side].set_active(price, true);
        sides[side].refresh_best_after_insert(side, price);
        version++;
    }

    // update level_sum and tail_pos
    p.level_sum = new_sum;
    p.tail_pos  = tail_pos;
}
