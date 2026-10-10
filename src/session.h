#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct Session {
  bool multiplayer=false, replay=false, custom_singleplayer=false;
  uint16_t game_type=0;
  unsigned self=8;
  std::array<uint8_t,8> players{};
  std::array<std::array<uint8_t,8>,8> alliances{};
  std::array<std::array<char,26>,8> names{}; // 25-byte UTF-8 legacy name field + NUL
  std::array<char,26> blocked_name{};        // set by session_opponent_rejection
};
constexpr size_t session_name_bytes=24; // ponytail: 25-byte field truncates long SCR names; prefix compare until the extended-name address is analysed
inline const char* session_opponent_name(const Session& s) {
  for(unsigned i=0;i<8;++i)if(i!=s.self && s.players[i]==2)return s.names[i].data();
  return "";
}
inline bool session_name_allowed(const char* candidate,const std::vector<std::string>& allowed) {
  for(const auto& name:allowed) {
    const size_t n=name.size()<session_name_bytes?name.size():session_name_bytes;
    if(n && std::strncmp(candidate,name.c_str(),n)==0 && (n==session_name_bytes || candidate[n]=='\0'))return true;
  }
  return false;
}
// Empty allowed list = everyone. Meant for multiplayer only; offline opponents are computers.
// With every_human (any-game mode) each other human player must be listed, not just the first.
// The first name that fails is left in blocked_name for the display message.
inline const char* session_opponent_rejection(Session& s,const std::vector<std::string>& allowed,bool every_human=false) {
  s.blocked_name={};
  if(allowed.empty())return nullptr;
  if(!every_human) {
    const char* opponent=session_opponent_name(s);
    if(session_name_allowed(opponent,allowed))return nullptr;
    std::memcpy(s.blocked_name.data(),opponent,25);return "opponent_not_allowed";
  }
  for(unsigned i=0;i<8;++i) {
    if(i==s.self || s.players[i]!=2)continue;
    if(session_name_allowed(s.names[i].data(),allowed))continue;
    std::memcpy(s.blocked_name.data(),s.names[i].data(),25);return "opponent_not_allowed";
  }
  return nullptr;
}
inline bool session_participant(uint8_t type) {return type==1 || type==2;}
inline unsigned session_participant_count(const Session& s) {
  unsigned count=0;for(auto type:s.players)count+=session_participant(type);return count;
}
inline bool session_has_local_ally(const Session& s) {
  if(s.self>=8)return false;
  for(unsigned i=0;i<8;++i)if(i!=s.self && session_participant(s.players[i]) &&
    (s.alliances[s.self][i] || s.alliances[i][s.self]))return true;
  return false;
}
inline const char* session_rejection(const Session& s,bool allow_multiplayer,bool challenge=false,bool any_game=false) {
  if(s.replay)return "replay";
  if(s.self>=8)return "local_observer";
  if(any_game) {
    // Permissive mode: any game type, any player mix, allies included. The
    // original 1v1 model still sees every other player as an enemy.
    if(s.players[s.self]!=2)return "local_slot_not_player";
    if(session_participant_count(s)<2)return "requires_opponent";
    if(s.multiplayer && !allow_multiplayer)return "multiplayer_not_enabled";
    return nullptr;
  }
  if(s.game_type!=2 && !(challenge && (s.game_type==3 || s.game_type==15)))return "unsupported_game_type";
  if(s.players[s.self]!=2)return "local_slot_not_player";
  unsigned participants=0,computers=0;
  for(auto type:s.players){participants+=session_participant(type);computers+=(type==1);}
  if(challenge ? participants<2 : participants!=2)return challenge?"requires_opponent":"requires_two_players";
  // The pinned observation builder labels every other active player as enemy;
  // human allies of Pluto would therefore be misclassified. Opponents may ally
  // with one another (e.g. the other side in Top vs Bottom).
  if(session_has_local_ally(s))return "local_allies_not_supported";
  if(s.multiplayer)return allow_multiplayer?nullptr:"multiplayer_not_enabled";
  if(!s.custom_singleplayer)return "requires_custom_game";
  if(computers!=(challenge?participants-1:1))return challenge?"offline_requires_computers":"offline_requires_one_computer";
  return nullptr;
}
inline bool supported_session(const Session& s,bool allow_multiplayer,bool challenge=false) {
  return session_rejection(s,allow_multiplayer,challenge)==nullptr;
}
inline const char* session_game_type(uint16_t type) {
  switch(type) {
    case 2:return "Melee";
    case 3:return "Free For All";
    case 4:return "One on One";
    case 10:return "Use Map Settings";
    case 15:return "Top vs Bottom";
    default:return "Other";
  }
}
