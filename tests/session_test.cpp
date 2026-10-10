#include "session.h"
#include <stdexcept>
#include <cstdio>
#include <cstring>
static void require(bool ok){if(!ok)throw std::runtime_error("Session admission test failed");}
int main(){
  Session s;s.self=3;s.game_type=2;s.players[3]=2;s.players[6]=1;s.custom_singleplayer=true;
  require(supported_session(s,false));
  s.multiplayer=true;s.custom_singleplayer=false;s.players[6]=2;
  require(!supported_session(s,false));require(supported_session(s,true));
  s.replay=true;require(!supported_session(s,true));s.replay=false;
  s.self=9;require(!supported_session(s,true));s.self=3;
  s.game_type=3;require(!supported_session(s,true));s.game_type=2;
  s.players[1]=2;require(!supported_session(s,true));s.players[1]=0;
  s.players[3]=1;require(!supported_session(s,true));s.players[3]=2;
  s.players[6]=0;require(!supported_session(s,true));
  // Failed user games were UMS (10), FFA (3), and Top vs Bottom (15),
  // each with exactly two human players. The diagnostic must name the cause.
  s.players[6]=2;
  for(auto type:{10,3,15}) {
    s.game_type=static_cast<uint16_t>(type);
    require(std::strcmp(session_rejection(s,true),"unsupported_game_type")==0);
    require(!supported_session(s,true));
  }
  s.game_type=2;require(session_rejection(s,true)==nullptr);
  require(std::strcmp(session_rejection(s,false),"multiplayer_not_enabled")==0);
  // Every local slot and 1..7 opponents, including non-contiguous slots. People
  // may ally with each other; any alliance with the bot is unsupported.
  for(unsigned self=0;self<8;++self)for(unsigned opponents=1;opponents<=7;++opponents) {
    Session many;many.self=self;many.players[self]=2;many.multiplayer=true;
    for(unsigned n=1;n<=opponents;++n)many.players[(self+n)%8]=2;
    require(session_participant_count(many)==opponents+1);
    for(auto type:{2,3,15}) {
      many.game_type=static_cast<uint16_t>(type);
      require(supported_session(many,true,true));
      require(!supported_session(many,false,true));
      if(opponents>1)require(!supported_session(many,true,false));
    }
    if(opponents>1) {
      const unsigned a=(self+1)%8,b=(self+2)%8;
      many.alliances[a][b]=many.alliances[b][a]=1;
      require(supported_session(many,true,true));
    }
    const unsigned opponent=(self+1)%8;
    many.alliances[self][opponent]=1;
    require(std::strcmp(session_rejection(many,true,true),"local_allies_not_supported")==0);
    many.alliances[self][opponent]=0;many.alliances[opponent][self]=1;
    require(!supported_session(many,true,true));
    many.alliances[opponent][self]=0;
    many.replay=true;require(!supported_session(many,true,true));many.replay=false;
    for(auto type:{10,11,12,13}) {many.game_type=static_cast<uint16_t>(type);require(!supported_session(many,true,true));}
    many.game_type=2;many.multiplayer=false;many.custom_singleplayer=true;
    require(!supported_session(many,false,true));
    for(unsigned i=0;i<8;++i)if(i!=self && many.players[i]==2)many.players[i]=1;
    require(supported_session(many,false,true));
  }
  // Opponent whitelist: empty list admits everyone; otherwise the 24-byte
  // prefix of the opponent's legacy name field must match an entry exactly.
  {
    Session w;w.self=0;w.players[0]=2;w.players[3]=2;w.multiplayer=true;w.game_type=2;
    std::strcpy(w.names[0].data(),"Me");std::strcpy(w.names[3].data(),"Friend#1234");
    require(std::strcmp(session_opponent_name(w),"Friend#1234")==0);
    require(session_opponent_rejection(w,{})==nullptr);
    require(session_opponent_rejection(w,{"Friend#1234"})==nullptr);
    require(session_opponent_rejection(w,{"Other","Friend#1234"})==nullptr);
    require(std::strcmp(session_opponent_rejection(w,{"Other"}),"opponent_not_allowed")==0);
    require(std::strcmp(session_opponent_rejection(w,{"Friend"}),"opponent_not_allowed")==0); // prefix alone is not a match
    require(std::strcmp(session_opponent_rejection(w,{"Me"}),"opponent_not_allowed")==0);     // own name does not admit
    require(std::strcmp(session_opponent_rejection(w,{""}),"opponent_not_allowed")==0);
    // Nine Hangul syllables = 27 UTF-8 bytes; the game field keeps 25, so only
    // the first 24 bytes (8 syllables) can be compared. A 9th syllable is unseen.
    const std::string hangul=u8"가나다라마바사아자";
    std::memcpy(w.names[3].data(),hangul.data(),25);w.names[3][25]='\0';
    require(session_opponent_rejection(w,{hangul})==nullptr);
    require(session_opponent_rejection(w,{u8"가나다라마바사아차"})==nullptr); // known limit: differs past byte 24
    require(std::strcmp(session_opponent_rejection(w,{u8"가나다라마바사차자"}),"opponent_not_allowed")==0);
    require(std::strncmp(w.blocked_name.data(),u8"가나다라마바사아",24)==0); // truncated field reported as seen
    // any-game team room: every other human must be listed; computers are ignored.
    Session t;t.self=2;t.multiplayer=true;t.game_type=15;
    t.players[2]=2;t.players[0]=2;t.players[5]=2;t.players[7]=1;
    std::strcpy(t.names[0].data(),"Ally");std::strcpy(t.names[5].data(),"Enemy");std::strcpy(t.names[7].data(),"Computer");
    require(session_opponent_rejection(t,{},true)==nullptr);
    require(session_opponent_rejection(t,{"Ally","Enemy"},true)==nullptr);
    require(std::strcmp(session_opponent_rejection(t,{"Ally"},true),"opponent_not_allowed")==0);
    require(std::strcmp(t.blocked_name.data(),"Enemy")==0);
    require(session_opponent_rejection(t,{"Ally"},false)==nullptr); // 1v1 rule only checks the first participant
  }
  // any_game: the user's failed Battle.net room (Top vs Bottom, 8 open slots)
  // and every other type/mix is admitted; only replay, observer, non-player
  // slot, no opponent and disabled multiplayer still block.
  {
    Session a;a.self=0;a.multiplayer=true;a.game_type=15;for(auto& t:a.players)t=2;
    require(std::strcmp(session_rejection(a,true,false,false),"unsupported_game_type")==0);
    require(session_rejection(a,true,false,true)==nullptr);
    require(std::strcmp(session_rejection(a,false,false,true),"multiplayer_not_enabled")==0);
    for(auto type:{2,3,4,10,15,99}){a.game_type=static_cast<uint16_t>(type);require(session_rejection(a,true,false,true)==nullptr);}
    a.alliances[0][1]=1;require(session_rejection(a,true,false,true)==nullptr); // allied bot admitted
    a.replay=true;require(std::strcmp(session_rejection(a,true,false,true),"replay")==0);a.replay=false;
    a.self=8;require(std::strcmp(session_rejection(a,true,false,true),"local_observer")==0);a.self=0;
    a.players[0]=1;require(std::strcmp(session_rejection(a,true,false,true),"local_slot_not_player")==0);a.players[0]=2;
    for(unsigned i=1;i<8;++i)a.players[i]=0;
    require(std::strcmp(session_rejection(a,true,false,true),"requires_opponent")==0);
    a.multiplayer=false;a.custom_singleplayer=false;a.players[1]=1;a.players[2]=1;a.game_type=10;
    require(session_rejection(a,false,false,true)==nullptr); // offline UMS vs two computers
  }
  Session alone;alone.game_type=15;alone.self=5;alone.players[5]=2;alone.multiplayer=true;
  require(std::strcmp(session_rejection(alone,true,true),"requires_opponent")==0);
  alone.players[1]=3;alone.alliances[5][1]=1;
  require(!session_has_local_ally(alone)); // Nonparticipant does not block play.
  std::puts("default 1v1 regressions and challenge 1v1..1v7, every slot, enemy teams, allied bot, UMS, shared-control teams, offline and multiplayer checks passed");
}
