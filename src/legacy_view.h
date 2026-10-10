#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <string>
#include "commands.h"
#include "unit_order.h"

// Compatibility view for the pinned Pluto binary's direct 1.16.1 memory reads.
// The bot writes only to this view; commands are collected for SCR translation.
class LegacyView {
public:
  LegacyView();
  void update();
  void bind(HMODULE pluto);
  void reset();
  void begin_frame(FILE* log,int frame);
  void discard_pending_turns(){pending_turns.clear();}
  unsigned drain(FILE* log,int frame);
  static void __cdecl flush_turn() noexcept;
  uint32_t scr_handle(uint16_t legacy_handle) const;
  // Players whose units are removed from Pluto's view (allies in any-game mode). Set by the bridge each frame.
  std::array<bool,8> hide_player{};
  // Per-update counters for diagnostics: units removed for hidden owners, units cut for the 1700-slot cap.
  unsigned hidden_units=0,dropped_units=0;
  // Extra cap on the active list Pluto sees (0 = only the 1700-slot table). Cuts inference time on crowded maps.
  unsigned max_active_units=0;
private:
  static constexpr uint32_t first=0x512000,last=0x6d2000,unit_base=0x59cca8;
  std::vector<uint8_t> memory;
  std::array<std::array<uint8_t,36>,1700> sprites{};
  std::vector<std::array<uint8_t,112>> bullets;
  std::vector<std::array<uint8_t,36>> bullet_sprites;
  struct Slot {uint32_t raw=0;uint8_t source_generation=0,generation=0;};
  std::array<Slot,1700> slots{};
  std::unordered_map<uint32_t,unsigned> lookup;
  uint32_t raw_start=0,raw_length=0;
  FILE* frame_log=nullptr;
  int frame_number=0;
  std::string flush_error;
  unsigned sent=0;
  struct PendingTurn {int frame;std::vector<Packet> packets;};
  std::vector<PendingTurn> pending_turns;
  uint32_t address(uint32_t old) const;
  uint32_t unit(uint32_t raw) const;
  uint16_t handle_for_raw(uint32_t raw) const;
  void sprite(uint32_t destination,uint32_t raw);
};
