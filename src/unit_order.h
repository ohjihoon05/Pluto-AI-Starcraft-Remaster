#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

// Pure ordering of the legacy active-unit list that Pluto observes. Pluto
// truncates its observation from the tail (max_units=768) and the legacy unit
// table holds 1700 slots, so the order decides what the model loses first.
struct UnitEntry {
  uint32_t raw=0;   // SCR unit address (opaque here)
  uint8_t owner=11; // player index; >=8 is neutral
  int32_t x=0,y=0;  // map pixels
};
struct OrderInput {
  unsigned self=0;
  std::array<bool,8> hidden{}; // players whose units are removed entirely (allies in any-game mode)
  int32_t home_x=0,home_y=0;   // own start position
  size_t capacity=1700;
};
inline unsigned unit_rank(const UnitEntry& u,const OrderInput& in) {
  if(u.owner==in.self)return 0;
  return u.owner<8?1:2; // other player (Pluto alliance 1) before neutral (alliance 2)
}
// Returns indices into `units` in legacy list order: own units first (original
// order), then other players' units and then neutrals, each by distance from
// home. Hidden owners are dropped first; what still exceeds `capacity` is cut
// from the tail (farthest neutrals, then farthest enemies). Own units are never cut.
// ponytail: O(n log n) sort every frame; bucket by rank if profiling shows it.
inline std::vector<size_t> order_units(const std::vector<UnitEntry>& units,const OrderInput& in,size_t* dropped=nullptr) {
  std::vector<size_t> kept;kept.reserve(units.size());
  for(size_t i=0;i<units.size();++i)if(!(units[i].owner<8 && in.hidden[units[i].owner]))kept.push_back(i);
  const auto key=[&](size_t i) {
    const auto& u=units[i];const unsigned rank=unit_rank(u,in);
    const int64_t dx=int64_t(u.x)-in.home_x,dy=int64_t(u.y)-in.home_y;
    return std::pair<unsigned,int64_t>(rank,rank?dx*dx+dy*dy:0);
  };
  std::stable_sort(kept.begin(),kept.end(),[&](size_t a,size_t b){return key(a)<key(b);});
  size_t cut=0;
  if(kept.size()>in.capacity){cut=kept.size()-in.capacity;kept.resize(in.capacity);}
  if(dropped)*dropped=cut;
  return kept;
}
