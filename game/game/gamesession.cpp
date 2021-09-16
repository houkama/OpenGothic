#include "gamesession.h"
#include "savegameheader.h"

#include <Tempest/Log>
#include <Tempest/MemReader>
#include <Tempest/MemWriter>
#include <cctype>

#include "worldstatestorage.h"
#include "world/objects/npc.h"
#include "world/objects/interactive.h"
#include "world/world.h"
#include "sound/soundfx.h"
#include "serialize.h"
#include "camera.h"
#include "gothic.h"

using namespace Tempest;

// rate 14.5 to 1
const uint64_t GameSession::multTime=29;
const uint64_t GameSession::divTime =2;

void GameSession::HeroStorage::save(Npc &npc,World& owner) {
  storage.clear();
  Tempest::MemWriter wr{storage};
  Serialize          sr{wr};
  sr.setContext(&owner);

  npc.save(sr);
  }

void GameSession::HeroStorage::putToWorld(World& owner,const std::string& wayPoint) const {
  if(storage.size()==0)
    return;
  Tempest::MemReader rd{storage};
  Serialize          sr{rd};
  sr.setContext(&owner);

  if(auto pl = owner.player()) {
    pl->load(sr);
    if(auto pos = owner.findPoint(wayPoint)) {
      pl->setPosition  (pos->x,pos->y,pos->z);
      pl->setDirection (pos->dirX,pos->dirY,pos->dirZ);
      pl->attachToPoint(pos);
      pl->updateTransform();
      }
    } else {
    auto ptr = std::make_unique<Npc>(owner,sr);
    owner.insertPlayer(std::move(ptr),wayPoint.c_str());
    }
  }


GameSession::GameSession(std::string file) {
  cam.reset(new Camera());

  if (Gothic::inst().isFreecamMode()) {
	  cam->setMode(Camera::Free);
  }

  Gothic::inst().setLoadingProgress(0);
  setTime(gtime(8,0));

  vm.reset(new GameScript(*this));
  setWorld(std::unique_ptr<World>(new World(*this,std::move(file),[&](int v){
    Gothic::inst().setLoadingProgress(int(v*0.55));
    })));

  vm->initDialogs();
  Gothic::inst().setLoadingProgress(70);

  const bool testMode=false;

  std::string_view hero = testMode ? "PC_ROCKEFELLER" : Gothic::inst().defaultPlayer();
  //std::string_view hero = "PC_ROCKEFELLER";
  //std::string_view hero = "FireGolem";
  //std::string_view hero = "Dragon_Undead";
  //std::string_view hero = "Sheep";
  //std::string_view hero = "Giant_Bug";
  //std::string_view hero = "OrcWarrior_Rest";
  //std::string_view hero = "Snapper";
  //std::string_view hero = "Lurker";
  //std::string_view hero = "Scavenger";
  //std::string_view hero = "StoneGolem";
  //std::string_view hero = "Waran";
  //std::string_view hero = "Bloodfly";
  //std::string_view hero = "Gobbo_Skeleton";
  wrld->createPlayer(hero);
  wrld->postInit();

  if(!testMode)
    initScripts(true);
  wrld->triggerOnStart(true);
  cam->reset(*wrld);
  Gothic::inst().setLoadingProgress(96);
  ticks = 1;
  // wrld->setDayTime(8,0);
  }

GameSession::GameSession(Serialize &fin) {
  cam.reset(new Camera());

  Gothic::inst().setLoadingProgress(0);
  uint16_t wssSize=0;

  SaveGameHeader hdr;
  fin.read(hdr,ticks,wrldTimePart);
  wrldTime = hdr.wrldTime;

  fin.read(wssSize);
  for(size_t i=0;i<wssSize;++i)
    visitedWorlds.emplace_back(fin);

  vm.reset(new GameScript(*this,fin));
  setWorld(std::unique_ptr<World>(new World(*this,fin,[&](int v){
    Gothic::inst().setLoadingProgress(int(v*0.55));
    })));

  vm->initDialogs();
  Gothic::inst().setLoadingProgress(70);
  wrld->load(fin);
  vm->loadVar(fin);
  if(auto hero = wrld->player())
    vm->setInstanceNPC("HERO",*hero);
  cam->load(fin,wrld->player());
  Gothic::inst().setLoadingProgress(96);
  }

GameSession::~GameSession() {
  }

void GameSession::save(Serialize &fout, const char* name, const Pixmap& screen) {
  SaveGameHeader hdr;
  hdr.name      = name;
  hdr.priview   = screen;
  hdr.world     = wrld->name();
  hdr.pcTime    = gtime::localtime();
  hdr.wrldTime  = wrldTime;
  hdr.isGothic2 = Gothic::inst().version().game;

  fout.write(hdr,ticks,wrldTimePart);
  fout.write(uint16_t(visitedWorlds.size()));

  Gothic::inst().setLoadingProgress(5);
  for(auto& i:visitedWorlds)
    i.save(fout);
  Gothic::inst().setLoadingProgress(25);

  vm->save(fout);
  Gothic::inst().setLoadingProgress(60);
  if(wrld)
    wrld->save(fout);

  Gothic::inst().setLoadingProgress(80);
  vm->saveVar(fout);
  cam->save(fout);
  }

void GameSession::setWorld(std::unique_ptr<World> &&w) {
  if(wrld) {
    if(!isWorldKnown(wrld->name()))
      visitedWorlds.emplace_back(*wrld);
    }
  wrld = std::move(w);
  }

std::unique_ptr<World> GameSession::clearWorld() {
  if(wrld) {
    if(!isWorldKnown(wrld->name())) {
      visitedWorlds.emplace_back(*wrld);
      }
    wrld->view()->setupUbo();
    }
  return std::move(wrld);
  }

void GameSession::changeWorld(const std::string& world, const std::string& wayPoint) {
  chWorld.zen = world;
  chWorld.wp  = wayPoint;
  }

void GameSession::exitSession() {
  exitSessionFlg=true;
  }

const VersionInfo& GameSession::version() const {
  return Gothic::inst().version();
  }

WorldView *GameSession::view() const {
  if(wrld)
    return wrld->view();
  return nullptr;
  }

Tempest::SoundEffect GameSession::loadSound(const Tempest::Sound &raw) {
  return sound.load(raw);
  }

Tempest::SoundEffect GameSession::loadSound(const SoundFx &fx, bool& looped) {
  return fx.getEffect(sound,looped);
  }

Npc* GameSession::player() {
  if(wrld)
    return wrld->player();
  return nullptr;
  }

void GameSession::updateListenerPos(Npc &npc) {
  auto plPos = npc.position();
  float rot = npc.rotationRad()+float(M_PI/2.0);
  float s   = std::sin(rot);
  float c   = std::cos(rot);
  sound.setListenerPosition(plPos.x,plPos.y+180/*head pos*/,plPos.z);
  sound.setListenerDirection(c,0,s, 0,1,0);
  }

void GameSession::setTime(gtime t) {
  wrldTime = t;
  }

void GameSession::tick(uint64_t dt) {
  wrld->scaleTime(dt);
  ticks+=dt;

  uint64_t add = (dt+wrldTimePart)*multTime;
  wrldTimePart=add%divTime;

  wrldTime.addMilis(add/divTime);

  wrld->tick(dt);
  // std::this_thread::sleep_for(std::chrono::milliseconds(60));

  if(exitSessionFlg) {
    exitSessionFlg = false;
    Gothic::inst().clearGame();
    Gothic::inst().onSessionExit();
    return;
    }

  if(!chWorld.zen.empty()) {
    char buf[128]={};
    for(auto& c:chWorld.zen)
      c = char(std::tolower(c));
    size_t beg = chWorld.zen.rfind('\\');
    size_t end = chWorld.zen.rfind('.');

    std::string wname;
    if(beg!=std::string::npos && end!=std::string::npos)
      wname = chWorld.zen.substr(beg+1,end-beg-1);
    else if(end!=std::string::npos)
      wname = chWorld.zen.substr(0,end); else
      wname = chWorld.zen;

    const char *w = (beg!=std::string::npos) ? (chWorld.zen.c_str()+beg+1) : chWorld.zen.c_str();

    if(Resources::vdfsIndex().hasFile(w)) {
      std::snprintf(buf,sizeof(buf),"LOADING_%s.TGA",wname.c_str());  // format load-screen name, like "LOADING_OLDWORLD.TGA"

      Gothic::inst().startLoad(buf,[this](std::unique_ptr<GameSession>&& game){
        auto ret = implChangeWorld(std::move(game),chWorld.zen,chWorld.wp);
        chWorld.zen.clear();
        return ret;
        });
      }
    }
  }

auto GameSession::implChangeWorld(std::unique_ptr<GameSession>&& game,
                                  const std::string& world, const std::string& wayPoint) -> std::unique_ptr<GameSession> {
  const char*   w   = world.c_str();
  size_t        cut = world.rfind('\\');
  if(cut!=std::string::npos)
    w = w+cut+1;

  if(!Resources::vdfsIndex().hasFile(w)) {
    Log::i("World not found[",world,"]");
    return std::move(game);
    }

  HeroStorage hdata;
  if(auto hero = wrld->player())
    hdata.save(*hero,*wrld);
  clearWorld();

  vm->resetVarPointers();

  const WorldStateStorage& wss = findStorage(w);

  auto loadProgress = [](int v){
    Gothic::inst().setLoadingProgress(v);
    };

  Tempest::MemReader rd{wss.storage.data(),wss.storage.size()};
  Serialize          fin = wss.isEmpty() ? Serialize::empty() : Serialize{rd};

  std::unique_ptr<World> ret;
  if(wss.isEmpty())
    ret = std::unique_ptr<World>(new World(*this,w,  loadProgress)); else
    ret = std::unique_ptr<World>(new World(*this,fin,loadProgress));
  setWorld(std::move(ret));

  if(!wss.isEmpty())
    wrld->load(fin);

  if(1){
    // put hero to world
    hdata.putToWorld(*game->wrld,wayPoint);
    }
  if(auto hero = wrld->player())
    vm->setInstanceNPC("HERO",*hero);

  initScripts(wss.isEmpty());
  wrld->triggerOnStart(wss.isEmpty());

  for(auto& i:visitedWorlds)
    if(i.name()==wrld->name()){
      i = std::move(visitedWorlds.back());
      visitedWorlds.pop_back();
      break;
      }

  cam->reset(*wrld);
  Log::i("Done loading world[",world,"]");
  return std::move(game);
  }

const WorldStateStorage& GameSession::findStorage(const std::string &name) {
  for(auto& i:visitedWorlds)
    if(i.name()==name)
      return i;
  static WorldStateStorage wss;
  return wss;
  }

void GameSession::updateAnimation(uint64_t dt) {
  if(wrld)
    wrld->updateAnimation(dt);
  }

std::vector<GameScript::DlgChoise> GameSession::updateDialog(const GameScript::DlgChoise &dlg, Npc& player, Npc& npc) {
  return vm->updateDialog(dlg,player,npc);
  }

void GameSession::dialogExec(const GameScript::DlgChoise &dlg, Npc& player, Npc& npc) {
  return vm->exec(dlg,player,npc);
  }

const Daedalus::ZString& GameSession::messageFromSvm(const Daedalus::ZString& id, int voice) const {
  if(!wrld){
    static Daedalus::ZString empty;
    return empty;
    }
  return vm->messageFromSvm(id,voice);
  }

const Daedalus::ZString& GameSession::messageByName(const Daedalus::ZString& id) const {
  if(!wrld){
    static Daedalus::ZString empty;
    return empty;
    }
  return vm->messageByName(id);
  }

uint32_t GameSession::messageTime(const Daedalus::ZString& id) const {
  if(!wrld)
    return 0;
  return vm->messageTime(id);
  }

AiOuputPipe *GameSession::openDlgOuput(Npc &player, Npc &npc) {
  AiOuputPipe* ret=nullptr;
  Gothic::inst().openDialogPipe(player, npc, ret);
  return ret;
  }

bool GameSession::aiIsDlgFinished() {
  return Gothic::inst().aiIsDlgFinished();
  }

bool GameSession::isWorldKnown(const std::string &name) const {
  for(auto& i:visitedWorlds)
    if(i.name()==name)
      return true;
  return false;
  }

void GameSession::initScripts(bool firstTime) {
  auto& wname = wrld->name();
  auto dot    = wname.rfind('.');
  auto name   = (dot==std::string::npos ? wname : wname.substr(0,dot));
  if( firstTime ) {
    std::string startup = "startup_"+name;

    if(vm->hasSymbolName(startup.c_str()))
      vm->runFunction(startup.c_str());
    }

  std::string init = "init_"+name;
  if(vm->hasSymbolName(init.c_str()))
    vm->runFunction(init.c_str());

  wrld->resetPositionToTA();
  }
