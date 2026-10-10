#include <Windows.h>
#include <MinHook.h>
#include <BWAPI.h>
#include <BWAPI/Client/GameImpl.h>
#include "scr_profile_13515_x86.h"
#include "scr_layout.h"
#include "snapshot.h"
#include "legacy_view.h"
#include "file_hash.h"
#include "overlay.h"
#include "session.h"
#include "messages.h"
#include <cstdio>
#include <share.h>
#include <intrin.h>
#include <filesystem>
#include <memory>
#include <exception>
#include <stdexcept>
#include <cctype>
#include <fstream>
#include <string>

namespace {
HMODULE bridge_module;
FILE* log_file;
DWORD (WINAPI* original_tick)();
std::unique_ptr<Snapshot> snapshot;
std::unique_ptr<LegacyView> legacy;
std::unique_ptr<BWAPI::GameImpl> api_game;
HMODULE bot_module;
BWAPI::AIModule* bot;
wchar_t bot_path[32768]{};
int last_frame=-1;
bool failed=false;
bool match_ended=false;
bool speed_changed=false;
bool unsupported_session_reported=false;
bool allow_multiplayer=false;
bool challenge_mode=false;
bool any_game=false;
bool no_resign=false;
unsigned max_units=0;
bool quiet=true; // in-game notices off unless bridge.ini show_messages=1; everything still goes to bridge.log
bool resign_reported=false;
bool view_stats_reported=false;
std::string opening;
std::array<bool,8> hidden_players{};
std::vector<std::string> allowed_opponents;
unsigned submitted_packets=0;
DWORD game_thread=0;
int speed_ms=0;
std::array<uint32_t,7> saved_speed{},saved_alt_speed{};
thread_local bool in_callback=false;

void print_local(const std::vector<std::string>& lines) {
  if(quiet)return;
  // SCR consumes UTF-8 here. Display only on this client, without network chat.
  for(const auto& line:lines)
    reinterpret_cast<void (__cdecl*)(const char*,uint32_t,uint32_t)>(scr::print_text())(line.c_str(),16,0);
}

Session read_session(uint32_t g) {
  Session session;
  session.multiplayer=scr::is_multiplayer()!=0;session.replay=scr::is_replay()!=0;
  session.custom_singleplayer=scr::read<uint8_t>(g+layout::Game::custom_singleplayer)==1;
  session.game_type=scr::read<uint16_t>(scr::addr(0x1240e58)+40);
  session.self=scr::local_player_id();
  for(unsigned i=0;i<8;++i) {
    session.players[i]=scr::read<uint8_t>(scr::players()+i*layout::Player::size+layout::Player::player_type);
    std::memcpy(session.names[i].data(),reinterpret_cast<void*>(scr::players()+i*layout::Player::size+layout::Player::name),25);
    for(unsigned j=0;j<8;++j)session.alliances[i][j]=scr::read<uint8_t>(g+layout::Game::alliances+i*12+j);
  }
  return session;
}

void set_speed(int value) {
  if(value<0 || scr::is_multiplayer())return;
  if(!speed_changed) {
    std::memcpy(saved_speed.data(),reinterpret_cast<void*>(scr::addr(0xfc4d7c)),28);
    std::memcpy(saved_alt_speed.data(),reinterpret_cast<void*>(scr::addr(0xfc4dac)),28);
    speed_changed=true;
  }
  for(unsigned n=0;n<7;++n) {
    *reinterpret_cast<uint32_t*>(scr::addr(0xfc4d7c)+n*4)=static_cast<uint32_t>(value);
    *reinterpret_cast<uint32_t*>(scr::addr(0xfc4dac)+n*4)=static_cast<uint32_t>(value)*3;
  }
}
void restore_speed() {
  if(!speed_changed)return;
  std::memcpy(reinterpret_cast<void*>(scr::addr(0xfc4d7c)),saved_speed.data(),28);
  std::memcpy(reinterpret_cast<void*>(scr::addr(0xfc4dac)),saved_alt_speed.data(),28);
  speed_changed=false;
}
void finish_match() {
  if(last_frame<0 || match_ended)return;
  match_ended=true;
  const bool won=scr::game() && scr::read<uint8_t>(scr::game()+58896+snapshot->data->self)==3;
  if(bot)bot->onEnd(won);
  clear_overlay();
  restore_speed();
  std::fprintf(log_file,"{\"stage\":\"match_end\",\"frame\":%d,\"winner\":%s,\"packets_submitted\":%u}\n",last_frame,won?"true":"false",submitted_packets);
  std::fflush(log_file);
}
// -Opening free: pre-seed Pluto's per-opponent bandit file so Thompson sampling
// picks the hint-free arm (mask 0). Uses Pluto's own key rule: first participant
// other than self, 25 bytes, characters outside [A-Za-z0-9_\-./] become '_'.
void seed_opening_free() {
  const auto self=scr::local_player_id();
  std::string name;
  for(unsigned i=0;i<8;++i) {
    const auto raw=scr::players()+i*layout::Player::size;
    const auto type=scr::read<uint8_t>(raw+layout::Player::player_type);
    if(i==self || (type!=1 && type!=2))continue;
    char buffer[26]{};std::memcpy(buffer,reinterpret_cast<void*>(raw+layout::Player::name),25);
    name=buffer;break;
  }
  for(auto& c:name)if(!(std::isalnum(static_cast<unsigned char>(c)) || c=='_' || c=='-' || c=='.' || c=='/'))c='_';
  if(name.empty())name="unknown";
  std::filesystem::create_directories("bwapi-data/write");
  const auto path="bwapi-data/write/pluto_bandit_"+name+".txt";
  unsigned long long seq=0;std::vector<std::string> records;
  if(std::ifstream in(path);in) {
    std::string line;
    while(std::getline(in,line)) {
      while(!line.empty() && (line.back()=='\r' || line.back()=='\n'))line.pop_back();
      if(line.rfind("seq ",0)==0)seq=std::strtoull(line.c_str()+4,nullptr,10);
      else if(!line.empty() && line[0]=='{')records.push_back(line);
    }
  }
  std::ofstream out(path+".tmp",std::ios::binary);
  out<<"# seeded by pluto-scr bridge: opening=free (mask 0x0 strongly preferred)\n";
  out<<"seq "<<(seq+1000)<<"\n";
  for(const char own:{'Z','T','P'})for(const char opp:{'Z','T','P','?'})out<<own<<' '<<opp<<" 0x00000000 100 0\n";
  for(const auto& record:records)out<<record<<"\n";
  out<<"end\n";out.close();
  MoveFileExA((path+".tmp").c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING);
  std::fprintf(log_file,"{\"stage\":\"opening_seeded\",\"file\":\"%s\",\"seq\":%llu}\n",path.c_str(),seq+1000);std::fflush(log_file);
}
void load_bot() {
  if(!bot_path[0])return;
  BWAPI::BWAPIClient.data=snapshot->data.get();
  api_game=std::make_unique<BWAPI::GameImpl>(snapshot->data.get());
  BWAPI::BroodwarPtr=api_game.get();
  api_game->onMatchStart();
  if(!bot_module) {
    if(file_sha256(bot_path)!=pluto_sha256)throw std::runtime_error("Unsupported Pluto DLL SHA-256; expected CoG 2026 release");
    bot_module=LoadLibraryExW(bot_path,nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!bot_module)throw std::runtime_error("Cannot load configured Pluto DLL");
    legacy=std::make_unique<LegacyView>();legacy->max_active_units=max_units;legacy->hide_player=hidden_players;legacy->update();legacy->bind(bot_module);
  }else{legacy->reset();legacy->hide_player=hidden_players;legacy->update();}
  if(opening=="free")seed_opening_free();
  const auto init=reinterpret_cast<void (__cdecl*)(BWAPI::Game*)>(GetProcAddress(bot_module,"gameInit"));
  const auto create=reinterpret_cast<BWAPI::AIModule* (__cdecl*)()>(GetProcAddress(bot_module,"newAIModule"));
  if(!init || !create)throw std::runtime_error("Missing BWAPI exports");
  init(api_game.get());bot=create();
  if(!bot)throw std::runtime_error("Pluto returned null");
  std::fprintf(log_file,"{\"stage\":\"bot_on_start_begin\"}\n");std::fflush(log_file);
  legacy->begin_frame(log_file,0);bot->onStart();
  set_speed(speed_ms);
  std::fprintf(log_file,"{\"stage\":\"bot_on_start_complete\"}\n");std::fflush(log_file);
}
void run_bot(bool first) {
  if(!bot)return;
  auto& d=*snapshot->data;
  legacy->hide_player=hidden_players;
  legacy->update();
  if(!view_stats_reported && (legacy->hidden_units || legacy->dropped_units)) {
    std::fprintf(log_file,"{\"stage\":\"view_filtered\",\"frame\":%d,\"hidden_units\":%u,\"dropped_units\":%u}\n",d.frameCount,legacy->hidden_units,legacy->dropped_units);
    view_stats_reported=true;
  }
  legacy->begin_frame(log_file,d.frameCount);
  if(!first)api_game->onMatchFrame();
  if(!first)for(const auto& e:api_game->getEvents()) {
    switch(e.getType()) {
      case BWAPI::EventType::UnitDiscover:bot->onUnitDiscover(e.getUnit());break;
      case BWAPI::EventType::UnitEvade:bot->onUnitEvade(e.getUnit());break;
      case BWAPI::EventType::UnitShow:bot->onUnitShow(e.getUnit());break;
      case BWAPI::EventType::UnitHide:bot->onUnitHide(e.getUnit());break;
      case BWAPI::EventType::UnitCreate:bot->onUnitCreate(e.getUnit());break;
      case BWAPI::EventType::UnitDestroy:bot->onUnitDestroy(e.getUnit());break;
      case BWAPI::EventType::UnitMorph:bot->onUnitMorph(e.getUnit());break;
      case BWAPI::EventType::UnitRenegade:bot->onUnitRenegade(e.getUnit());break;
      case BWAPI::EventType::UnitComplete:bot->onUnitComplete(e.getUnit());break;
      default:break;
    }
  }
  bot->onFrame();
  submitted_packets+=legacy->drain(log_file,d.frameCount);
  for(int i=0;i<d.commandCount;++i) {
    auto& c=d.commands[i];
    if((c.type==BWAPIC::CommandType::Printf || c.type==BWAPIC::CommandType::SendText) && c.value1>=0 && c.value1<d.stringCount) {
      std::fprintf(log_file,"bot_text: %s\n",d.strings[c.value1]);
      print_local(pluto_messages_ko(d.strings[c.value1]));
    }
    else std::fprintf(log_file,"{\"game_command\":%d,\"v1\":%d,\"v2\":%d}\n",c.type,c.value1,c.value2);
    if(c.type==BWAPIC::CommandType::EnableFlag && c.value1>=0 && c.value1<BWAPI::Flag::Max)d.flags[c.value1]=true;
    // The launcher's explicit speed policy takes priority over Pluto's onStart
    // request to uncap. A negative setting leaves the game's own speed intact.
    if(c.type==BWAPIC::CommandType::SetLocalSpeed)set_speed(speed_ms);
    if(c.type==BWAPIC::CommandType::LeaveGame && no_resign) {
      // Pluto resigns when its win estimate stays pinned low; in team or custom
      // games that estimate is unreliable, so keep playing and ignore the request.
      if(!resign_reported) {
        resign_reported=true;
        std::fprintf(log_file,"{\"stage\":\"resign_suppressed\",\"frame\":%d}\n",d.frameCount);
        print_local({u8"플루토가 기권하려 했지만 계속 플레이합니다 (-NoResign)."});
      }
      continue;
    }
    if(c.type==BWAPIC::CommandType::LeaveGame) {
      // Multiplayer departure must use the game's normal loop teardown. Do not
      // change simulation victory state locally, which peers would not observe.
      if(!scr::is_multiplayer())*reinterpret_cast<uint8_t*>(scr::game()+58896+d.self)=1;
      *reinterpret_cast<uint8_t*>(scr::addr(0x121758d))=0;
      finish_match();
    }
  }
  for(int i=0;i<d.unitCommandCount;++i) {
    auto& c=d.unitCommands[i];
    std::fprintf(log_file,"{\"decision_frame\":%d,\"command\":%d,\"unit\":%d,\"target\":%d,\"x\":%d,\"y\":%d,\"extra\":%d,\"executed\":false}\n",d.frameCount,c.type.getID(),c.unitIndex,c.targetIndex,c.x,c.y,c.extra);
  }
  update_overlay(d);
  d.commandCount=d.unitCommandCount=d.shapeCount=d.stringCount=0;
  std::fflush(log_file);
}

void update_frame() {
  if((scr::is_multiplayer() && !allow_multiplayer) || scr::is_replay())return;
  const auto g=scr::game();
  if(!g || scr::local_player_id()>=8)return;
  const auto frame=static_cast<int>(scr::read<uint32_t>(g+332));
  if(frame<last_frame || (match_ended && frame==0)) {
    finish_match();
    if(api_game)api_game->onMatchEnd();
    BWAPI::BroodwarPtr=nullptr;BWAPI::BWAPIClient.data=nullptr;
    api_game.reset();snapshot.reset();bot=nullptr;
    last_frame=-1;match_ended=false;submitted_packets=0;resign_reported=false;view_stats_reported=false;
  }
  if(match_ended)return;
  if(frame==last_frame)return;
  auto session=read_session(g);
  for(unsigned i=0;i<8;++i)hidden_players[i]=any_game && i!=session.self && session.self<8 && session.players[i]!=0 &&
    (session.alliances[session.self][i]!=0 || session.alliances[i][session.self]!=0);
  if(last_frame>=0 && !any_game && session_has_local_ally(session)) {
    // A new alliance cannot be represented by the original model. Stop this
    // match's AI and discard commands queued before the alliance changed.
    if(legacy)legacy->discard_pending_turns();
    print_local({u8"플루토 조종 중단: 플루토와 동맹인 플레이어가 생겼습니다.",
      u8"다음 경기는 플루토를 혼자 한 팀에 배치해 주세요."});
    std::fprintf(log_file,"{\"stage\":\"ai_stopped\",\"reason\":\"local_alliance_changed\",\"frame\":%d}\n",frame);
    finish_match();return;
  }
  if(last_frame<0) {
    auto reason=session_rejection(session,allow_multiplayer,challenge_mode,any_game);
    if(!reason && session.multiplayer)reason=session_opponent_rejection(session,allowed_opponents,any_game);
    if(reason) {
      if(!unsupported_session_reported) {
        std::fprintf(log_file,"{\"stage\":\"session_skipped\",\"reason\":\"%s\",\"game_type\":%u,\"game_type_name\":\"%s\",\"self\":%u,\"multiplayer\":%s,\"multiplayer_enabled\":%s,\"opponent\":\"%s\",\"player_types\":[",
          reason,session.game_type,session_game_type(session.game_type),session.self,
          session.multiplayer?"true":"false",allow_multiplayer?"true":"false",session_opponent_name(session));
        for(unsigned i=0;i<8;++i)std::fprintf(log_file,"%s%u",i?",":"",session.players[i]);
        std::fprintf(log_file,"]}\n");
        std::fflush(log_file);unsupported_session_reported=true;
        print_local(session_messages_ko(session,reason,challenge_mode));
      }
      return;
    }
    unsupported_session_reported=false;
    std::fprintf(log_file,"{\"stage\":\"session_accepted\",\"multiplayer\":%s,\"self\":%u,\"game_type\":%u,\"participants\":%u,\"challenge\":%s,\"any_game\":%s,\"opponent\":\"%s\"}\n",
      session.multiplayer?"true":"false",session.self,session.game_type,session_participant_count(session),challenge_mode?"true":"false",any_game?"true":"false",session_opponent_name(session));
    if(any_game) {
      std::vector<std::string> lines{u8"플루토 무제한 모드: "+std::string(session_game_type(session.game_type))+u8", 참가자 "+std::to_string(session_participant_count(session))+u8"명."};
      if(session_has_local_ally(session))lines.push_back(u8"동맹 플레이어의 유닛은 플루토 관측에서 제외합니다 (공격 안 함).");
      print_local(lines);
    }
    if(challenge_mode)print_local({u8"플루토 콘텐츠 실험 모드: 1 대 "+std::to_string(session_participant_count(session)-1)+u8" 경기입니다.",
      u8"1대1 모델을 사용하므로 다인전 전투력과 승률 예측은 검증 전입니다."});
  }
  if(!snapshot)snapshot=std::make_unique<Snapshot>();
  if(!snapshot->update(last_frame<0))return;
  if(last_frame<0)load_bot();
  run_bot(last_frame<0);
  auto& d=*snapshot->data;
  if(frame<3 || frame%24==0) {
    std::fprintf(log_file,"{\"frame\":%d,\"delta\":%d,\"map_width\":%d,\"map_height\":%d,\"self\":%d,\"race\":%d,\"minerals\":%d,\"initial_units\":%d,\"events\":%d,\"packets_submitted\":%u}\n",frame,frame-last_frame,d.mapWidth,d.mapHeight,d.self,d.players[d.self].race,d.players[d.self].minerals,d.initialUnitCount,d.eventCount,submitted_packets);
    std::fflush(log_file);
  }
  last_frame=frame;
}
void guarded_frame_cpp(bool frame) {
  try {
    if(last_frame>=0 && !scr::read<uint8_t>(scr::addr(0x121758d)))finish_match();
    if(frame)update_frame();
  }
  catch(const std::exception& e){failed=true;restore_speed();clear_overlay();std::fprintf(log_file,"C++ failure: %s\n",e.what());std::fflush(log_file);}
}
LONG diagnose(EXCEPTION_POINTERS* p) {
  failed=true;
  restore_speed();
  std::fprintf(log_file,"{\"failure\":\"bridge_exception\",\"code\":%lu,\"address\":\"%p\",\"pluto_rva\":%lu,\"access\":%lu,\"target\":\"%08lx\"}\n",p->ExceptionRecord->ExceptionCode,p->ExceptionRecord->ExceptionAddress,p->ContextRecord->Eip-reinterpret_cast<DWORD>(bot_module),p->ExceptionRecord->ExceptionInformation[0],p->ExceptionRecord->ExceptionInformation[1]);
  std::fflush(log_file);return EXCEPTION_EXECUTE_HANDLER;
}
void guarded_frame(bool frame) {
  __try {guarded_frame_cpp(frame);}
  __except(diagnose(GetExceptionInformation())){}
}
DWORD WINAPI tick_hook() {
  const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
  // Verified call sites in step_game_logic: before its first frame and after each
  // completed simulation frame. Restrict interception to those two callers.
  const bool frame=caller==scr::addr(0x76e02f) || caller==scr::addr(0x77d112);
  if(!failed && !in_callback && (frame || (game_thread && game_thread==GetCurrentThreadId()))) {
    if(frame)game_thread=GetCurrentThreadId();
    in_callback=true;guarded_frame(frame);in_callback=false;
  }
  return original_tick();
}
DWORD WINAPI initialize(void*) {
  wchar_t own_path[32768]{};
  GetModuleFileNameW(bridge_module,own_path,32768);
  const auto directory=std::filesystem::path(own_path).parent_path();
  GetPrivateProfileStringW(L"pluto",L"module",L"",bot_path,32768,(directory/L"bridge.ini").c_str());
  speed_ms=GetPrivateProfileIntW(L"pluto",L"speed_ms",0,(directory/L"bridge.ini").c_str());
  allow_multiplayer=GetPrivateProfileIntW(L"pluto",L"multiplayer",0,(directory/L"bridge.ini").c_str())==1;
  challenge_mode=GetPrivateProfileIntW(L"pluto",L"challenge",0,(directory/L"bridge.ini").c_str())==1;
  any_game=GetPrivateProfileIntW(L"pluto",L"any_game",0,(directory/L"bridge.ini").c_str())==1;
  no_resign=GetPrivateProfileIntW(L"pluto",L"no_resign",0,(directory/L"bridge.ini").c_str())==1;
  quiet=GetPrivateProfileIntW(L"pluto",L"show_messages",0,(directory/L"bridge.ini").c_str())!=1;
  max_units=GetPrivateProfileIntW(L"pluto",L"max_units",0,(directory/L"bridge.ini").c_str());
  {
    wchar_t value[32]{};GetPrivateProfileStringW(L"pluto",L"opening",L"auto",value,32,(directory/L"bridge.ini").c_str());
    opening.clear();for(const wchar_t* c=value;*c;++c)opening+=static_cast<char>(*c); // ascii keywords only
  }
  // Pluto writes pluto.log and bwapi-data/ relative to the process CWD. A game
  // started by the Battle.net app (-Attach) has an arbitrary CWD; pin it here.
  SetCurrentDirectoryW(directory.c_str());
  if(speed_ms>1000)speed_ms=1000;
  {
    // Comma-separated opponent account names; empty = everyone. Compared as UTF-8 against the in-game name field.
    wchar_t raw[4096]{};
    GetPrivateProfileStringW(L"pluto",L"opponents",L"",raw,4096,(directory/L"bridge.ini").c_str());
    std::wstring entry;
    for(const wchar_t* p=raw;;++p) {
      if(*p && *p!=L',') {entry+=*p;continue;}
      while(!entry.empty() && entry.back()==L' ')entry.pop_back();
      size_t start=0;while(start<entry.size() && entry[start]==L' ')++start;
      if(start<entry.size()) {
        const auto size=WideCharToMultiByte(CP_UTF8,0,entry.c_str()+start,-1,nullptr,0,nullptr,nullptr);
        std::string utf8(size>0?size-1:0,'\0');
        if(size>1)WideCharToMultiByte(CP_UTF8,0,entry.c_str()+start,-1,utf8.data(),size,nullptr,nullptr);
        if(!utf8.empty())allowed_opponents.push_back(utf8);
      }
      entry.clear();
      if(!*p)break;
    }
  }
  {
    // Pluto reads BWRL_* through msvcrt getenv, which snapshots the environment
    // when msvcrt loads. A game started by the Battle.net app inherits nothing
    // from the launcher, so publish the launcher's settings into that same CRT
    // before pluto.dll is loaded. Absent ini keys leave the process environment alone.
    HMODULE crt=LoadLibraryW(L"msvcrt.dll");
    const auto putenv_=crt?reinterpret_cast<int (__cdecl*)(const char*)>(GetProcAddress(crt,"_putenv")):nullptr;
    const wchar_t* keys[][2]={{L"draw",L"BWRL_DRAW"},{L"straddle",L"BWRL_STRADDLE"},{L"frame_budget_ms",L"BWRL_FRAME_BUDGET_MS"}};
    for(const auto& key:keys) {
      wchar_t value[64]{};
      if(!GetPrivateProfileStringW(L"pluto",key[0],L"",value,64,(directory/L"bridge.ini").c_str()) || !value[0])continue;
      char assignment[128]{};
      std::snprintf(assignment,sizeof assignment,"%ls=%ls",key[1],value);
      SetEnvironmentVariableW(key[1],value);
      if(putenv_)putenv_(assignment);
    }
  }
  log_file=_wfsopen((directory/"bridge.log").c_str(),L"w",_SH_DENYNO);
  if(!log_file)return 1;
  std::fprintf(log_file,"{\"stage\":\"initializing\",\"pid\":%lu,\"mode\":\"%s\",\"challenge\":%s,\"any_game\":%s,\"no_resign\":%s,\"opening\":\"%s\",\"max_units\":%u,\"allowed_opponents\":%zu}\n",GetCurrentProcessId(),
    challenge_mode?"challenge_experimental":(allow_multiplayer?"multiplayer_experimental":"offline_pluto"),challenge_mode?"true":"false",any_game?"true":"false",no_resign?"true":"false",opening.c_str(),max_units,allowed_opponents.size());std::fflush(log_file);
  try {
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
    if(file_sha256(executable)!=scr_sha256)throw std::runtime_error("Unsupported StarCraft executable SHA-256");
  }catch(const std::exception& e){std::fprintf(log_file,"Initialization rejected: %s\n",e.what());std::fflush(log_file);return 2;}
  const auto target=scr::read<uintptr_t>(scr::addr(0xdd01c4));
  const auto expected=reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetTickCount"));
  std::fprintf(log_file,"{\"timing_import\":\"%p\",\"winapi\":\"%p\"}\n",reinterpret_cast<void*>(target),reinterpret_cast<void*>(expected));
  auto result=MH_Initialize();
  if(result==MH_OK)result=MH_CreateHook(reinterpret_cast<void*>(expected),&tick_hook,reinterpret_cast<void**>(&original_tick));
  if(result==MH_OK)result=MH_EnableHook(reinterpret_cast<void*>(expected));
  std::fprintf(log_file,"{\"stage\":\"hook_install\",\"result\":\"%s\"}\n",MH_StatusToString(result));std::fflush(log_file);
  return result==MH_OK?0:3;
}
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
  if(reason==DLL_PROCESS_ATTACH){bridge_module=module;DisableThreadLibraryCalls(module);HANDLE thread=CreateThread(nullptr,0,initialize,nullptr,0,nullptr);if(!thread)return FALSE;CloseHandle(thread);}
  return TRUE;
}
