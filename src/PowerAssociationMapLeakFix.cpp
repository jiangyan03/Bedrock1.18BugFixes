// PowerAssociationMapLeakFix.cpp
#include "PowerAssociationMapLeakFix.h"

#include <HookAPI.h>
#include <LoggerAPI.h>
#include <MC/BaseCircuitComponent.hpp>
#include <MC/BlockPos.hpp>
#include <MC/CircuitSceneGraph.hpp>

Logger PowerAssociationMapLeakFix::logger("PowerAssociationMapLeakFix");

// 原版函数指针占位
static void(__fastcall *orig_removeStale)(CircuitSceneGraph *);

namespace PowerAssociationMapLeakFix {

// CircuitSceneGraph 内偏移
static constexpr size_t OFF_mAllComponents          = 0x00;
static constexpr size_t OFF_mPowerAssociationMap    = 0x98;
static constexpr size_t OFF_mPendingUpdates_relList = 0x120;

// unordered_map 内部偏移
static constexpr ptrdiff_t OFF_mask    = 0x30;
static constexpr ptrdiff_t OFF_Maxidx  = 0x28;
static constexpr ptrdiff_t _Equal_func = 0x20;
static constexpr ptrdiff_t OFF_buckets = 0x18;
static constexpr ptrdiff_t OFF_size    = 0x10;
static constexpr ptrdiff_t OFF_end     = 0x08;

// hash3 
using Hash3Func = uint64_t (*)(const int &, const int &, const int &);
static Hash3Func computeHash3 = nullptr;

// 从内存中快速查条目
static uint64_t lookupHashMap(CircuitSceneGraph *scene, size_t tableOffset,
                              const BlockPos &key) {
  auto base = reinterpret_cast<uint8_t *>(scene) + tableOffset;
  auto maskPtr = reinterpret_cast<uint64_t *>(base + OFF_mask);
  auto bucketsPtr = reinterpret_cast<void **>(base + OFF_buckets);
  auto endNode = reinterpret_cast<uint64_t *>(base + OFF_end);
  if (!maskPtr || !bucketsPtr || !endNode)
    return 0;
  uint64_t mask = *maskPtr;
  if (!computeHash3 || mask == 0)
    return 0;
  uint64_t h = computeHash3(key.x, key.y, key.z) & mask;
  auto buckets = reinterpret_cast<uint64_t *>(*bucketsPtr);
  auto *bucket = buckets + h * 2;
  if (!bucket[0] || bucket == endNode)
    return 0;

  // 从 tail 向 head 搜
  uint64_t ent = bucket[1], head = bucket[0];
  while (ent && ent != head) {
    BlockPos np;
    std::memcpy(&np, reinterpret_cast<void *>(ent + 0x10), sizeof(np));
    if (np.x == key.x && np.y == key.y && np.z == key.z)
      return ent;
    ent = *reinterpret_cast<uint64_t *>(ent + 8);
  }
  // 检查 tail
  if (head) {
      BlockPos pos;
      std::memcpy(&pos, reinterpret_cast<void*>(head + 0x10), sizeof(pos));
      if (pos.x==key.x && pos.y==key.y && pos.z==key.z) {
          return head;
      }
  }
  return 0;
}

// 模拟 std::vector::erase(it) 的行为，适用于裸内存 vector（定长元素）
inline uint8_t* eraseVectorEntry(uint8_t*& start, uint8_t*& finish, uint8_t* it, size_t ENTRY = 32) {
    // assert(it >= start && it < finish);
    uint8_t* next = it + ENTRY;
    if (next < finish) {
        std::memmove(it, next, finish - next);
    }
    finish -= ENTRY;
    return it;
}

// 钩子：补清残留
static void __fastcall hooked_removeStaleRelationships(CircuitSceneGraph *scene) {
  // 不执行原版
  __try {
    // 拿到 pendingUpdates 哨兵
    uint64_t sentinel = *reinterpret_cast<uint64_t *>(
        reinterpret_cast<char *>(scene) + OFF_mPendingUpdates_relList);
    if (!sentinel) return;
    // 遍历每个更新项
    for (uint64_t cur = *reinterpret_cast<uint64_t *>(sentinel);
        cur && cur != sentinel; cur = *reinterpret_cast<uint64_t *>(cur)) 
    {
      BlockPos posUpdate;
      std::memcpy(&posUpdate, reinterpret_cast<void *>(cur + 0x10),sizeof(posUpdate));
      logger.error("posUpdate.x{},posUpdate.y{},posUpdate.z{}",posUpdate.x, posUpdate.y, posUpdate.z);
      auto rawComp = *reinterpret_cast<BaseCircuitComponent **>(reinterpret_cast<char *>(cur) + 0x20);
      if (!rawComp) continue;
      // 在 PowerAssociationMap中找那个 vector 结构
      uint64_t entPower = lookupHashMap(scene, OFF_mPowerAssociationMap, posUpdate);
      if (!entPower) continue;

      // 清空 mComponent
      constexpr size_t    ENTRY   = 32;
      constexpr ptrdiff_t OFF_VEC = 0x20;
      auto*  base       = reinterpret_cast<uint8_t*>(entPower) + OFF_VEC;
      auto** pStartPtr  = reinterpret_cast<uint8_t**>(base + 0x00);
      auto** pFinishPtr = reinterpret_cast<uint8_t**>(base + 0x08);
      uint8_t*& start  = *pStartPtr;
      uint8_t*& finish = *pFinishPtr;

      for (uint8_t* it = start; it < finish; it = eraseVectorEntry(start, finish, it, ENTRY)) {
        BlockPos chunkPos;
        std::memcpy(&chunkPos, it + 12, sizeof(chunkPos)); // mPos 偏移 12
        // logger.error("chunkPos.x{},chunkPos.y{},chunkPos.z{}",chunkPos.x, chunkPos.y, chunkPos.z);
        // 调用 removeSource
        if (uint64_t entAll = lookupHashMap(scene, OFF_mAllComponents, chunkPos)) {
            auto uptrPtr = reinterpret_cast<std::unique_ptr<BaseCircuitComponent>*>(entAll + 0x20);
            if (uptrPtr && uptrPtr->get())
                uptrPtr->get()->removeSource(posUpdate, rawComp);
                // logger.error("清理成功chunkPos.x{},chunkPos.y{},chunkPos.z{}",chunkPos.x, chunkPos.y, chunkPos.z);
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    orig_removeStale(scene);
    logger.error("PowerAssociationMap 额外清理时发生异常，已跳过补充逻辑");
  }
}

// 安装钩子
bool installHook() {
  //  mce::Math::hash3
  computeHash3 = reinterpret_cast<Hash3Func>(dlsym_real("??$hash3@HHH@Math@mce@@SA_KAEBH00@Z"));
  if (!computeHash3) {
      logger.error("找不到 mce::Math::hash3 符号，补丁无法安装");
      return false;
  }
  void *addr =dlsym_real("?removeStaleRelationships@CircuitSceneGraph@@AEAAXXZ");
  if (!addr) {
    logger.error("找不到 removeStaleRelationships 符号");
    return false;
  }
  HookFunction(addr, reinterpret_cast<void **>(&orig_removeStale),
               reinterpret_cast<void *>(&hooked_removeStaleRelationships));
  logger.info("PowerAssociationMapLeakFix 钩子安装完成 @0x{:X}",(uintptr_t)addr);
  return true;
}

} // namespace PowerAssociationMapLeakFix
