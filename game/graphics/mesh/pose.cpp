#include "pose.h"

#include <Tempest/Log>

#include "world/objects/npc.h"
#include "world/world.h"
#include "game/serialize.h"
#include "utils/fileext.h"
#include "skeleton.h"
#include "animmath.h"

#include <cmath>

using namespace Tempest;

uint8_t Pose::calcAniComb(const Vec3& dpos, float rotation) {
  float   l   = std::sqrt(dpos.x*dpos.x+dpos.z*dpos.z);

  float   dir = 90+180.f*std::atan2(dpos.z,dpos.x)/float(M_PI);
  float   aXZ = (rotation-dir);
  float   aY  = -std::atan2(dpos.y,l)*180.f/float(M_PI);

  uint8_t cx  = (aXZ<-30.f) ? 0 : (aXZ<=30.f ? 1 : 2);
  uint8_t cy  = (aY <-45.f) ? 0 : (aY <=45.f ? 1 : 2);

  // sides angle: +/- 30 height angle: +/- 45
  return uint8_t(1u+cy*3u+cx);
  }

uint8_t Pose::calcAniCombVert(const Vec3& dpos) {
  float   l  = std::sqrt(dpos.x*dpos.x+dpos.z*dpos.z);
  float   aY = 180.f*std::atan2(dpos.y,l)/float(M_PI);
  uint8_t cy = (aY <-25.f) ? 0 : (aY <=25.f ? 1 : 2);

  if(dpos.y<-50)
    cy = 0;
  if(dpos.y>50)
    cy = 2;

  // height angle: +/- 25
  return uint8_t(cy+1u);
  }

void Pose::save(Serialize &fout) {
  uint8_t sz=uint8_t(lay.size());
  fout.write(sz);
  for(auto& i:lay) {
    fout.write(i.seq->name,i.sAnim,i.bs);
    }
  fout.write(lastUpdate);
  fout.write(combo.bits);
  fout.write(rotation ? rotation->name : "");
  fout.write(itemUseSt,itemUseDestSt);
  fout.write(headRotX,headRotY);

  for(auto& i:base)
    fout.write(i);
  for(auto& i:tr)
    fout.write(i);
  }

void Pose::load(Serialize &fin, const AnimationSolver& solver) {
  std::string name;
  uint8_t     sz = uint8_t(lay.size());

  fin.read(sz);
  lay.resize(sz);
  for(auto& i:lay) {
    fin.read(name,i.sAnim,i.bs);
    i.seq = solver.solveFrm(name);
    }
  fin.read(lastUpdate);
  fin.read(combo.bits);
  removeIf(lay,[](const Layer& l){
    return l.seq==nullptr;
    });
  fin.read(name);
  for(auto& i:lay) {
    if(i.seq->name==name)
      rotation = i.seq;
    }
  fin.read(itemUseSt,itemUseDestSt);
  for(auto& i:lay)
    onAddLayer(i);
  fin.read(headRotX,headRotY);
  needToUpdate = true;

  numBones = skeleton==nullptr ? 0 : skeleton->nodes.size();
  for(auto& i:base)
    fin.read(i);
  for(auto& i:tr)
    fin.read(i);
  }

void Pose::setFlags(Pose::Flags f) {
  flag = f;
  }

BodyState Pose::bodyState() const {
  uint32_t b = BS_NONE;
  for(auto& i:lay)
    b = std::max(b,uint32_t(i.bs&(BS_MAX | BS_FLAG_MASK)));
  return BodyState(b);
  }

void Pose::setSkeleton(const Skeleton* sk) {
  if(skeleton==sk)
    return;
  skeleton = sk;
  if(skeleton!=nullptr) {
    numBones = skeleton->tr.size();
    for(size_t i=0; i<numBones; ++i) {
      tr[i]   = skeleton->tr[i];
      base[i] = ZenLoad::zCModelAniSample{}; //mkSample(skeleton->nodes[i].tr);
      }
    } else {
    numBones = 0;
    }

  trY = skeleton->rootTr.y;

  if(lay.size()>0) //TODO
    Log::d("WARNING: ",__func__," animation adjustment not implemented");
  lay.clear();
  }

bool Pose::startAnim(const AnimationSolver& solver, const Animation::Sequence *sq, uint8_t comb, BodyState bs,
                     StartHint hint, uint64_t tickCount) {
  if(sq==nullptr)
    return false;
  // NOTE: zero stands for no-comb, other numbers are comb-index+1
  comb = std::min<uint8_t>(comb, uint8_t(sq->comb.size()));

  const bool force = (hint&Force);

  for(auto& i:lay)
    if(i.seq->layer==sq->layer) {
      const bool hasNext   = (!i.seq->next.empty() && i.seq->animCls!=Animation::Loop);
      const bool finished  = i.seq->isFinished(tickCount,i.sAnim,combo.len()) && !hasNext;
      const bool interrupt = force || i.seq->canInterrupt(tickCount,i.sAnim,combo.len());
      if(i.seq==sq && i.comb==comb && i.bs==bs && !finished)
        return true;
      if(!interrupt && !finished)
        return false;
      if(i.bs==BS_ITEMINTERACT) {
        stopItemStateAnim(solver,tickCount);
        return false;
        }
      char tansition[256]={};
      const Animation::Sequence* tr=nullptr;
      if(i.seq->shortName!=nullptr && sq->shortName!=nullptr) {
        std::snprintf(tansition,sizeof(tansition),"T_%s_2_%s",i.seq->shortName,sq->shortName);
        tr = solver.solveFrm(tansition);
        }
      if(tr==nullptr && sq->shortName!=nullptr) {
        std::snprintf(tansition,sizeof(tansition),"T_STAND_2_%s",sq->shortName);
        tr = solver.solveFrm(tansition);
        }
      if(tr==nullptr && i.seq->shortName!=nullptr && sq->isIdle()) {
        std::snprintf(tansition,sizeof(tansition),"T_%s_2_STAND",i.seq->shortName);
        tr = solver.solveFrm(tansition);
        }
      onRemoveLayer(i);
      i.seq   = tr ? tr : sq;
      i.sAnim = tickCount;
      i.comb  = comb;
      i.bs    = bs;
      onAddLayer(i);
      return true;
      }
  addLayer(sq,bs,comb,tickCount);
  return true;
  }

bool Pose::stopAnim(std::string_view name) {
  bool done=false;
  size_t ret=0;
  for(size_t i=0;i<lay.size();++i) {
    bool rm = (name.empty() || lay[i].seq->name==name);
    if(itemUseSt!=0 && lay[i].bs==BS_ITEMINTERACT)
      rm = false;

    if(!rm) {
      if(ret!=i)
        lay[ret] = lay[i];
      ret++;
      } else {
      onRemoveLayer(lay[i]);
      done=true;
      }
    }
  lay.resize(ret);
  return done;
  }

bool Pose::stopWalkAnim() {
  bool done=false;
  size_t ret=0;
  for(size_t i=0;i<lay.size();++i) {
    if(lay[i].bs!=BS_RUN && lay[i].bs!=BS_SPRINT && lay[i].bs!=BS_SNEAK && lay[i].bs!=BS_WALK) {
      if(ret!=i)
        lay[ret] = lay[i];
      ret++;
      } else {
      onRemoveLayer(lay[i]);
      done=true;
      }
    }
  lay.resize(ret);
  return done;
  }

void Pose::interrupt() {
  size_t ret=0;
  for(size_t i=0;i<lay.size();++i) {
    if(BS_FLAG_INTERRUPTABLE & lay[i].bs) {
      onRemoveLayer(lay[i]);
      } else {
      if(ret!=i)
        lay[ret] = lay[i];
      ret++;
      }
    }
  lay.resize(ret);
  }

void Pose::stopAllAnim() {
  for(auto& i:lay)
    onRemoveLayer(i);
  lay.clear();
  }

void Pose::processLayers(AnimationSolver& solver, uint64_t tickCount) {
  size_t ret    = 0;
  bool   doSort = false;
  for(size_t i=0; i<lay.size(); ++i) {
    const auto& l = lay[i];
    if(l.seq->animCls==Animation::Transition && l.seq->isFinished(tickCount,l.sAnim,combo.len())) {
      auto next = solveNext(solver,lay[i]);
      if(next!=lay[i].seq) {
        needToUpdate = true;
        onRemoveLayer(lay[i]);

        if(next!=nullptr) {
          if(lay[i].seq==rotation)
            rotation = next;
          doSort       = lay[i].seq->layer!=next->layer;
          lay[i].seq   = next;
          lay[i].sAnim = tickCount;
          onAddLayer(lay[i]);
          ret++;
          }
        continue;
        }
      }
    if(ret!=i)
      lay[ret] = lay[i];
    ret++;
    }
  lay.resize(ret);

  if(doSort) {
    std::sort(lay.begin(),lay.end(),[](const Layer& a,const Layer& b){
      return a.seq->layer<b.seq->layer;
      });
    }
  }

bool Pose::update(uint64_t tickCount) {
  if(lay.size()==0){
    if(lastUpdate==0) {
      zeroSkeleton();
      needToUpdate = false;
      lastUpdate   = tickCount;
      return true;
      }
    const bool ret = needToUpdate;
    needToUpdate = false;
    lastUpdate   = tickCount;
    return ret;
    }

  if(lastUpdate!=tickCount) {
    for(auto& i:lay) {
      const Animation::Sequence* seq = i.seq;
      if(0<i.comb && i.comb<=i.seq->comb.size()) {
        if(auto sx = i.seq->comb[size_t(i.comb-1)])
          seq = sx;
        }
      needToUpdate |= updateFrame(*seq,lastUpdate,i.sAnim,tickCount);
      }
    lastUpdate = tickCount;
    }

  if(needToUpdate) {
    mkSkeleton(*lay[0].seq,lay[0].bs);
    needToUpdate = false;
    return true;
    }
  return false;
  }

bool Pose::updateFrame(const Animation::Sequence &s,
                       uint64_t barrier, uint64_t sTime, uint64_t now) {
  auto&        d         = *s.data;
  const size_t numFrames = d.numFrames;
  const size_t idSize    = d.nodeIndex.size();
  if(numFrames==0 || idSize==0 || d.samples.size()%idSize!=0)
    return false;
  if(numFrames==1 && !needToUpdate)
    return false;

  (void)barrier;
  now = now-sTime;

  float    fpsRate = d.fpsRate;
  uint64_t frame   = uint64_t(float(now)*fpsRate);
  uint64_t frameA  = frame/1000;
  uint64_t frameB  = frame/1000+1; //next

  float    a       = float(frame%1000)/1000.f;

  if(s.animCls==Animation::Loop){
    frameA%=d.numFrames;
    frameB%=d.numFrames;
    } else {
    frameA = std::min<uint64_t>(frameA,d.numFrames-1);
    frameB = std::min<uint64_t>(frameB,d.numFrames-1);
    }

  if(s.reverse) {
    frameA = d.numFrames-1-frameA;
    frameB = d.numFrames-1-frameB;
    }

  auto* sampleA = &d.samples[size_t(frameA*idSize)];
  auto* sampleB = &d.samples[size_t(frameB*idSize)];

  for(size_t i=0; i<idSize; ++i) {
    size_t idx = d.nodeIndex[i];
    if(idx>=numBones)
      continue;
    base[idx] = mix(sampleA[i],sampleB[i],a);
    }
  return true;
  }

void Pose::mkSkeleton(const Animation::Sequence &s, BodyState bs) {
  if(skeleton==nullptr)
    return;
  Matrix4x4 m = mkBaseTranslation(&s,bs);
  if(skeleton->ordered)
    mkSkeleton(m); else
    mkSkeleton(m,size_t(-1));
  }

void Pose::mkSkeleton(const Matrix4x4 &mt) {
  if(skeleton==nullptr)
    return;
  auto& nodes      = skeleton->nodes;
  auto  BIP01_HEAD = skeleton->BIP01_HEAD;
  for(size_t i=0; i<nodes.size(); ++i) {
    size_t parent = nodes[i].parent;
    auto   mat    = base[i].rotation.w==0 ? nodes[i].tr : mkMatrix(base[i]);

    if(parent<Resources::MAX_NUM_SKELETAL_NODES)
      tr[i] = tr[parent]*mat; else
      tr[i] = mt*mat;

    if(i==BIP01_HEAD && (headRotX!=0 || headRotY!=0)) {
      Matrix4x4& m = tr[i];
      m.rotateOY(headRotY);
      m.rotateOX(headRotX);
      }
    }
  }

void Pose::mkSkeleton(const Tempest::Matrix4x4 &mt, size_t parent) {
  if(skeleton==nullptr)
    return;
  auto& nodes = skeleton->nodes;
  for(size_t i=0;i<nodes.size();++i){
    if(nodes[i].parent!=parent)
      continue;
    auto mat = base[i].rotation.w==0 ? nodes[i].tr : mkMatrix(base[i]);
    tr[i] = mt*mat;
    mkSkeleton(tr[i],i);
    }
  }

const Animation::Sequence* Pose::solveNext(const AnimationSolver &solver, const Layer& lay) {
  auto sq = lay.seq;

  if((lay.bs & BS_ITEMINTERACT)==BS_ITEMINTERACT && itemUseSt!=itemUseDestSt) {
    int sA = itemUseSt, sB = itemUseSt, nextState = itemUseSt;
    if(itemUseSt<itemUseDestSt) {
      sB++;
      nextState = itemUseSt+1;
      } else {
      sB--;
      nextState = itemUseSt-1;
      }
    char scheme[64]={};
    sq->schemeName(scheme);

    const Animation::Sequence* ret = nullptr;
    if(itemUseSt>itemUseDestSt) {
      char T_ID_SX_2_STAND[128]={};
      std::snprintf(T_ID_SX_2_STAND,sizeof(T_ID_SX_2_STAND),"T_%s_S%d_2_STAND",scheme,itemUseSt);
      ret = solver.solveFrm(T_ID_SX_2_STAND);
      }

    if(ret==nullptr) {
      char T_ID_Sa_2_Sb[256]={};
      std::snprintf(T_ID_Sa_2_Sb,sizeof(T_ID_Sa_2_Sb),"T_%s_S%d_2_S%d",scheme,sA,sB);
      ret = solver.solveFrm(T_ID_Sa_2_Sb);
      }

    if(ret==nullptr && itemUseDestSt>=0)
      return sq;
    itemUseSt = nextState;
    return ret;
    }

  return solver.solveNext(*sq);
  }

void Pose::addLayer(const Animation::Sequence *seq, BodyState bs, uint8_t comb, uint64_t tickCount) {
  if(seq==nullptr)
    return;
  Layer l;
  l.seq   = seq;
  l.sAnim = tickCount;
  l.bs    = bs;
  l.comb  = comb;
  lay.push_back(l);
  onAddLayer(lay.back());
  std::sort(lay.begin(),lay.end(),[](const Layer& a,const Layer& b){
    return a.seq->layer<b.seq->layer;
    });
  }

void Pose::onAddLayer(const Pose::Layer& l) {
  if(hasLayers(l))
    hasEvents++;
  if(l.seq->isFly())
    isFlyCombined++;
  needToUpdate = true;
  }

void Pose::onRemoveLayer(const Pose::Layer &l) {
  if(l.seq==rotation)
    rotation=nullptr;
  if(hasLayers(l))
    hasEvents--;
  if(l.seq->isFly())
    isFlyCombined--;
  }

bool Pose::hasLayers(const Pose::Layer& l) {
  return
      l.seq->data->events.size()>0 ||
      l.seq->data->mmStartAni.size()>0 ||
      l.seq->data->gfx.size()>0;
  }

void Pose::processSfx(Npc &npc, uint64_t tickCount) {
  for(auto& i:lay)
    i.seq->processSfx(lastUpdate,i.sAnim,tickCount,npc);
  }

void Pose::processPfx(MdlVisual& visual, World& world, uint64_t tickCount) {
  for(auto& i:lay)
    i.seq->processPfx(lastUpdate,i.sAnim,tickCount,visual,world);
  }

bool Pose::processEvents(uint64_t &barrier, uint64_t now, Animation::EvCount &ev) const {
  if(hasEvents>0) {
    for(auto& i:lay)
      i.seq->processEvents(barrier,i.sAnim,now,ev);
    }
  barrier=now;
  return hasEvents>0;
  }

Tempest::Vec3 Pose::animMoveSpeed(uint64_t tickCount,uint64_t dt) const {
  Tempest::Vec3 ret;
  for(auto& i:lay) {
    if(!i.seq->data->hasMoveTr && i.seq->animCls!=Animation::Transition)
      continue;
    ret += i.seq->speed(tickCount-i.sAnim,dt);
    if(i.bs==BS_RUN)
      return ret;
    }
  return ret;
  }

bool Pose::isDefParWindow(uint64_t tickCount) const {
  for(auto& i:lay)
    if(i.seq->isDefParWindow(tickCount-i.sAnim))
      return true;
  return false;
  }

bool Pose::isDefWindow(uint64_t tickCount) const {
  for(auto& i:lay)
    if(i.seq->isDefWindow(tickCount-i.sAnim))
      return true;
  return false;
  }

bool Pose::isDefence(uint64_t tickCount) const {
  char buf[32]={};
  static const char* alt[3]={"","_A2","_A3"};

  for(auto& i:lay) {
    if(i.seq->isDefWindow(tickCount-i.sAnim)) {
      // FIXME: seems like name check is not needed
      for(int h=1;h<=2;++h) {
        for(int v=0;v<3;++v) {
          std::snprintf(buf,sizeof(buf),"T_%dHPARADE_0%s",h,alt[v]);
          if(i.seq->name==buf)
            return true;
          }
        }
      }
    }
  return false;
  }

bool Pose::isJumpBack() const {
  char buf[32]={};
  for(auto& i:lay) {
    for(int h=1;h<=2;++h) {
      std::snprintf(buf,sizeof(buf),"T_%dHJUMPB",h);
      if(i.seq->name==buf)
        return true;
      }
    if(i.seq->name=="T_FISTPARADEJUMPB")
      return true;
    }
  return false;
  }

bool Pose::isJumpAnim() const {
  for(auto& i:lay) {
    if(i.bs!=BS_JUMP || i.seq->animCls!=Animation::Transition)
      continue;
    return true;
    }
  return false;
  }

bool Pose::isFlyAnim() const {
  return isFlyCombined>0;
  }

bool Pose::isStanding() const {
  if(lay.size()!=1 || lay[0].seq->animCls==Animation::Transition)
    return false;
  auto& s = *lay[0].seq;
  if(s.isIdle())
    return true;
  // check common idle animations
  return s.name=="S_FISTRUN"  || s.name=="S_MAGRUN"  ||
         s.name=="S_1HRUN"    || s.name=="S_BOWRUN"  ||
         s.name=="S_2HRUN"    || s.name=="S_CBOWRUN" ||
         s.name=="S_RUN"      || s.name=="S_WALK"    ||
         s.name=="S_FISTWALK" || s.name=="S_MAGWALK" ||
         s.name=="S_1HWALK"   || s.name=="S_BOWWALK" ||
         s.name=="S_2HWALK"   || s.name=="S_CBOWWALK";
  }

bool Pose::isPrehit(uint64_t now) const {
  for(auto& i:lay)
    if(i.seq->isPrehit(i.sAnim,now))
      return true;
  return false;
  }

bool Pose::isAtackAnim() const {
  for(auto& i:lay)
    if(i.seq->isAtackAnim())
      return true;
  return false;
  }

bool Pose::isIdle() const {
  for(auto& i:lay)
    if(!i.seq->isIdle())
      return false;
  return true;
  }

bool Pose::isInAnim(std::string_view sq) const {
  for(auto& i:lay)
    if(i.seq->name==sq)
      return true;
  return false;
  }

bool Pose::isInAnim(const Animation::Sequence *sq) const {
  for(auto& i:lay)
    if(i.seq==sq)
      return true;
  return false;
  }

bool Pose::hasAnim() const {
  return lay.size()>0;
  }

uint64_t Pose::animationTotalTime() const {
  uint64_t ret=0;
  for(auto& i:lay)
    ret = std::max(ret,uint64_t(i.seq->totalTime()));
  return ret;
  }

const Animation::Sequence* Pose::continueCombo(const AnimationSolver &solver, const Animation::Sequence *sq,
                                               uint64_t tickCount) {
  if(sq==nullptr)
    return nullptr;

  Layer* prev = nullptr;
  for(auto& i:lay)
    if(i.seq->layer==sq->layer) {
      prev = &i;
      break;
      }

  if(prev==nullptr) {
    combo = ComboState();
    return nullptr;
    }

  auto&    d  = *prev->seq->data;
  uint64_t t  = tickCount-prev->sAnim;
  size_t   id = combo.len()*2;

  if(0==d.defWindow.size() || 0==d.defHitEnd.size()) {
    if(!startAnim(solver,sq,prev->comb,prev->bs,Pose::NoHint,tickCount))
      return nullptr;
    combo = ComboState();
    return sq;
    }

  if(sq->data->defHitEnd.size()==0) {
    // hit -> block
    startAnim(solver,sq,prev->comb,prev->bs,Pose::Force,tickCount);
    combo = ComboState();
    return sq;
    }

  if(id+1>=d.defWindow.size()) {
    combo.setBreak();
    return nullptr;
    }

  if(t<d.defWindow[id+0] || d.defWindow[id+1]<=t) {
    if(prev->seq->name==sq->name)
      combo.setBreak();
    return nullptr;
    }

  if(combo.isBreak())
    return nullptr;

  if(prev->seq->name!=sq->name) {
    startAnim(solver,sq,prev->comb,prev->bs,Pose::Force,tickCount);
    combo = ComboState();
    return sq;
    }

  if(combo.len()<d.defHitEnd.size())
    prev->sAnim = tickCount - d.defHitEnd[combo.len()];
  combo.incLen();
  return prev->seq;
  }

uint16_t Pose::comboLength() const {
  return combo.len();
  }

const Tempest::Matrix4x4& Pose::bone(size_t id) const {
  return tr[id];
  }

size_t Pose::boneCount() const {
  return numBones;
  }

size_t Pose::findNode(std::string_view b) const {
  if(skeleton!=nullptr)
    return skeleton->findNode(b);
  return size_t(-1);
  }

void Pose::setHeadRotation(float dx, float dz) {
  headRotX = dx;
  headRotY = dz;
  }

Vec2 Pose::headRotation() const {
  return Vec2(headRotX,headRotY);
  }

void Pose::setAnimRotate(const AnimationSolver &solver, Npc &npc, WeaponState fightMode, int dir) {
  const Animation::Sequence *sq = nullptr;
  if(dir==0) {
    if(rotation!=nullptr) {
      if(stopAnim(rotation->name.c_str()))
        rotation = nullptr;
      }
    return;
    }
  if(bodyState()!=BS_STAND)
    return;
  if(dir<0) {
    sq = solver.solveAnim(AnimationSolver::Anim::RotL,fightMode,npc.walkMode(),*this);
    } else {
    sq = solver.solveAnim(AnimationSolver::Anim::RotR,fightMode,npc.walkMode(),*this);
    }
  if(rotation!=nullptr) {
    if(sq!=nullptr && rotation->name==sq->name)
      return;
    if(!stopAnim(rotation->name.c_str()))
      return;
    }
  if(sq==nullptr)
    return;
  if(startAnim(solver,sq,0,BS_FLAG_FREEHANDS,Pose::NoHint,npc.world().tickCount())) {
    rotation = sq;
    return;
    }
  }

bool Pose::setAnimItem(const AnimationSolver &solver, Npc &npc, std::string_view scheme, int state) {
  char T_ID_STAND_2_S0[128]={};
  std::snprintf(T_ID_STAND_2_S0,sizeof(T_ID_STAND_2_S0),"T_%.*s_STAND_2_S0",int(scheme.size()),scheme.data());
  const Animation::Sequence *sq = solver.solveFrm(T_ID_STAND_2_S0);
  if(startAnim(solver,sq,0,BS_ITEMINTERACT,Pose::NoHint,npc.world().tickCount())) {
    itemUseSt     = 0;
    itemUseDestSt = state;
    return true;
    }
  return false;
  }

bool Pose::stopItemStateAnim(const AnimationSolver& solver, uint64_t tickCount) {
  if(itemUseSt<0)
    return true;
  itemUseDestSt = -1;
  for(auto& i:lay)
    if(i.bs==BS_ITEMINTERACT) {
      auto next = solveNext(solver,i);
      if(next==nullptr)
        continue;
      onRemoveLayer(i);
      i.seq   = next;
      i.sAnim = tickCount;
      onAddLayer(i);
      }
  return true;
  }

const Matrix4x4* Pose::transform() const {
  return tr;
  }

Matrix4x4 Pose::mkBaseTranslation(const Animation::Sequence *s, BodyState bs) {
  Matrix4x4 m;
  m.identity();

  if(numBones==0)
    return m;

  size_t id=0;
  if(skeleton->rootNodes.size())
    id = skeleton->rootNodes[0];
  auto& nodes = skeleton->nodes;
  auto  b0 = base[id].rotation.w==0 ? nodes[id].tr : mkMatrix(base[id]);
  float dx = b0.at(3,0);
  float dy = 0;
  float dz = b0.at(3,2);

  if((flag&NoTranslation)==NoTranslation)
    dy = b0.at(3,1);
  else if(bs==BS_CLIMB)
    dy = b0.at(3,1) - trY;
  else if(bs==BS_DIVE)
    dy = b0.at(3,1);// - 50;
  else if(s!=nullptr && s->isFly())
    dy = b0.at(3,1) - (s->data->translate.y);
  else
    dy = 0;

  m.translate(-dx,-dy,-dz);
  return m;
  }

void Pose::zeroSkeleton() {
  if(skeleton==nullptr)
    return;
  auto& nodes = skeleton->tr;
  Matrix4x4 m = mkBaseTranslation(nullptr,BS_NONE);
  for(size_t i=0;i<nodes.size();++i){
    tr[i] = m * nodes[i];
    }
  }

template<class T, class F>
void Pose::removeIf(T &t, F f) {
  size_t ret=0;
  for(size_t i=0;i<t.size();++i) {
    if( !f(t[i]) ) {
      if(ret!=i)
        t[ret] = t[i];
      ret++;
      }
    }
  t.resize(ret);
  }
