#pragma once
#include <array>
#include <unordered_map>
#include <numeric>
#include <cassert>
#include "phase.hpp"
#include "types.hpp"
#include "addr.hpp"

struct LineSumMap { // a temporary substitution of LineSumCache, write immediate, actually not RTL structure!!!
  std::unordered_map<uint64_t, std::array<QtySum_t,16>> ram;  // key = line_key

  void reset(){ram.clear();}

  void apply_delta(LineSumCacheView addr, QtyDelta_t delta) // write-through
  {
    assert(LineSumCacheViewValid(addr));
    const uint64_t key = get_LineSum_key(addr);
    const uint64_t offset = addr.offset;
    //if(ram.find(key) == ram.end() && delta == 0)return;
    const QtySum_t current = ram[key][offset];
    assert(current < (1ULL << QtySum_bits));
    const QtySum_t nxt = current + QtySum_t(delta); // must not exceed QtySum_bits + 1 since delta {delta sign bit, delta unsigned bits = QtySum_bits}
    assert(nxt < (1ULL << QtySum_bits)); // if delta > 0 and too positive, exceeds QtySum_bits, then assert fail; if delta < 0 and delta < -current, then nxt become negative meaning, QtySum_bits + 1 bit become 1, then assert failed too
    ram[key][offset] = nxt;
    for(size_t j = 0; j < 16; j++)if (ram[key][j] != 0) return; //if all clear, then delete from map
    ram.erase(ram.find(key));
  }

  std::array<QtySum_t,16> read_row_through(LineSumCacheView addr)                     // read through !!!
  {
    assert(LineSumCacheViewValid(addr));
    const uint64_t key = get_LineSum_key(addr);
    if(ram.find(key) == ram.end())return std::array<QtySum_t, 16>();
    else return ram[key];
  }

  QtySum_t read_single_through(LineSumCacheView addr)                // read through !!!
  {
    assert(LineSumCacheViewValid(addr));
    const uint64_t key = get_LineSum_key(addr);
    const uint64_t offset = addr.offset;
    if(ram.find(key) == ram.end())return 0;
    else return ram[key][offset];
  }

  void clear() // does not map to reset() in RTL, debug only
  {
      ram.clear();
  }
};

struct LevelSumMap { // a temporary substitution of LevelSumCache, write immediate, actually not RTL structure!!!
  std::unordered_map<uint64_t, std::array<QtySum_t,16>> ram;  // key = line_key

  void reset(){ram.clear();}

  void apply_delta(LevelSumCacheView addr, QtyDelta_t delta) // write-through
  {
    assert(LevelSumCacheViewValid(addr));
    const uint64_t key = get_LevelSum_key(addr);
    const uint64_t offset = addr.offset;
    //if(ram.find(key) == ram.end() && delta == 0)return;
    const QtySum_t current = ram[key][offset];
    assert(current < (1ULL << QtySum_bits));
    const QtySum_t nxt = current + QtySum_t(delta); // must not exceed QtySum_bits + 1 since delta {delta sign bit, delta unsigned bits = QtySum_bits}
    assert(nxt < (1ULL << QtySum_bits)); // if delta > 0 and too positive, exceeds QtySum_bits, then assert fail; if delta < 0 and delta < -current, then nxt become negative meaning, QtySum_bits + 1 bit become 1, then assert failed too
    ram[key][offset] = nxt;
    for(size_t j = 0; j < 16; j++)if (ram[key][j] != 0) return; //if all clear, then delete from map
    ram.erase(ram.find(key));
  }

  std::array<QtySum_t,16> read_row_through(LevelSumCacheView addr)                     // read through !!!
  {
    assert(LevelSumCacheViewValid(addr));
    const uint64_t key = get_LevelSum_key(addr);
    if(ram.find(key) == ram.end())return std::array<QtySum_t, 16>();
    else return ram[key];
  }

  QtySum_t read_single_through(LevelSumCacheView addr)                // read through !!!
  {
    assert(LevelSumCacheViewValid(addr));
    const uint64_t key = get_LevelSum_key(addr);
    const uint64_t offset = addr.offset;
    if(ram.find(key) == ram.end())return 0;
    else return ram[key][offset];
  }

  void clear() // does not map to reset() in RTL, debug only
  {
    ram.clear();
  }
};
/*
 constexpr 不能修饰函数参数、for (size_t i = R - 1; i >= 0; i--) 是无符号死循环、read() 还是 ... 占位、ram[k] 会插入空行、signed/unsigned 混加。
template <size_t W, size_t R>
struct LineSumCache
{
    std::array<std::array<QtySum_t, 16>, 32> slots;                  // 一行 16 槽 reg
    std::array<bool, 32> valid;                                      // reg
    std::array<LineSum_tag_t, 32> tag;                               // reg
    std::unordered_map<LineSum_key_t, std::array<QtySum_t, 16>> ram; // 下层（稀疏，全零即删） reg

    std::array<LineSumCacheView, W> pending_addr  ;                       // wire
    std::array<QtyDelta_t, W>       pending_deltas;                       // wire
    std::array<bool, W>             pending_valid ;                       // wire
    std::array<bool, W>             w_port_driven ;                       // wire

    std::array<LineSum_key_t, R>            fill_request  ; // wire
    std::array<bool, R>                     fill_valid    ; // wire
    std::array<bool, R>                     r_port_driven ;                       // wire
  
    //void reset();
    //void prepare();
    //void commit();
  
    //void apply_delta(constexpr size_t port, LineSumCacheView &addr, QtyDelta_t delta); // 写穿+命中镜
    //QtySum_t read(constexpr size_t port, LineSumCacheView &addr);                      // if miss, 填充窗口
    //QtySum_t read_ram(LineSumCacheView &addr);                  // for debug only, will not trigger filling
    //std::array<QtySum_t, 16> read_row_ram(LineSum_key_t &key);  // for debug only, will not trigger filling

    void reset()
    {
      slots = std::array<std::array<QtySum_t, 16>, 32>();
      valid.fill(false);
      tag.fill(0);
      ram = std::unordered_map<LineSum_key_t, std::array<QtySum_t, 16>>();
    }

    void prepare()
    {
      assert(global_phase == Phase::Prepare);
      pending_addr  .fill(0);
      pending_deltas.fill(0);
      pending_valid .fill(false);
      w_port_driven.fill(false);
      fill_request.fill(0);
      fill_valid.fill(false);
      r_port_driven.fill(false);
      //pending_RAM_addr    .fill(0);
      //pending_RAM_deltas  .fill(0);
      //pending_RAM_valid   .fill(false);
      //fill_do   .fill(0);
      //fill_index.fill(0);
      //fill_data .fill(std::array<QtySum_t, 16>());
    }

    void commit(){
      assert(global_phase == Phase::Commit);
      for (size_t i = R - 1; i >= 0; i--){ // fill in lower port is priortized and will cover fillinf in upper port
        if(fill_valid[i]){
          std::array<QtySum_t, 16> value = (ram.find(fill_request[i]) == ram.end())? std::array<QtySum_t, 16>() : ram[fill_request[i]];
          //if key exists in ram, use ram, if not, use new empty array
          const LineSum_index_t idx = LineSum_key2index(fill_request[i]);
          const LineSum_tag_t   t   = LineSum_key2tag  (fill_request[i]);
          tag[idx] = t;
          slots[idx] = value;
          valid[idx] = true;
        }
      }
      for (size_t i = W - 1; i >= 0; i--){ // write in lower port is priortized and will cover the write in upper port
        if(pending_valid[i]){
          const LineSum_index_t idx = pending_addr[i].index;
          if(valid[idx] && tag[idx] == pending_addr[i].tag ){ // srote hit, updating cached value
            slots[idx][pending_addr[i].offset] += pending_deltas[i];
          }
        }
      }
      for (size_t i = W - 1; i >= 0; i--){ // write in lower port is priortized and will cover the write in upper port
        if(pending_valid[i]){
          const LineSum_key_t k   = get_LineSum_key(pending_addr[i]);
          QtySum_t sum = 0;
          for(size_t j = 0; j < 16; j++)sum += ram[k][j];
          if(sum + pending_deltas[i] == 0)ram.erase(ram.find(k));
          else ram[k][pending_addr[i].offset] += pending_deltas[i];
        }
      }
    }

    void apply_delta(constexpr size_t port, LineSumCacheView &addr, QtyDelta_t delta){
      assert(global_phase == Phase::Comb);
      assert(port < W);
      assert(!w_port_driven[port]);
      w_port_driven [port] = true;
      pending_addr  [port] = addr;
      pending_deltas[port] = delta;
      pending_valid [port] = true;
    }

    QtySum_t read(constexpr size_t port, LineSumCacheView &addr){
      assert(global_phase == Phase::Comb);
      assert(port < R);
      assert(!r_port_driven[port]);
      r_port_driven[port] = true;
      bool hit = addr.tag == tag[addr.index] && valid[addr.index];
      if(!hit){
        fill_request[port] = get_LineSum_key(addr);
        fill_valid[port] = true;
        return ...
      }
      return ...
    }

    QtySum_t read_ram(LineSumCacheView &addr){
      std::array<QtySum_t, 16> line = (ram.find(get_LineSum_key(addr)) == ram.end())? std::array<QtySum_t, 16>() : ram[get_LineSum_key(addr)];
      return line[addr.offset];
    }
    std::array<QtySum_t, 16> read_row_ram(LineSum_key_t &key){
        return (ram.find(key) == ram.end())? std::array<QtySum_t, 16>() : ram[key];
    }
};
*/
