// PowerAssociationMapLeakFix.cpp
#include "PowerAssociationMapLeakFix.h"

#include <HookAPI.h>
#include <LoggerAPI.h>
#include <MC/BaseCircuitComponent.hpp>
#include <MC/BlockPos.hpp>
#include <MC/CircuitSceneGraph.hpp>

// 原版函数指针占位
static void(__fastcall *orig_removeStale)(CircuitSceneGraph *);
// update(BlockSource*)
// static void (__fastcall *orig_update)(CircuitSceneGraph *, BlockSource *);

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

// 模拟查条目
static uint64_t lookupHashMap(CircuitSceneGraph *scene, size_t tableOffset,
                              const BlockPos &key) {
  auto base = reinterpret_cast<uint8_t *>(scene) + tableOffset;
  auto maskPtr = reinterpret_cast<uint64_t *>(base + OFF_mask);
  auto bucketsPtr = reinterpret_cast<void **>(base + OFF_buckets);
  auto endNode = reinterpret_cast<uint64_t *>(base + OFF_end);
  auto sizePtr = reinterpret_cast<uint64_t *>(base + OFF_size);
  if (!maskPtr || !bucketsPtr || !endNode ||!sizePtr)
    return 0;
  uint64_t mask = *maskPtr;
  uint64_t h = computeHash3(key.x, key.y, key.z) & mask;
  auto buckets = reinterpret_cast<uint64_t *>(*bucketsPtr);
  auto *bucket = buckets + h * 2;
  if (!bucket[0] || bucket == endNode)
    return 0;
  // 从 ent 向 head 搜
  // uint64_t entry_count = 0;
  uint64_t ent = bucket[1], head = bucket[0];
  while (ent && ent != head) {
    BlockPos np;
    std::memcpy(&np, reinterpret_cast<void *>(ent + 0x10), sizeof(np));
    if (np.x == key.x && np.y == key.y && np.z == key.z)
      return ent;
    // entry_count++;
    ent = *reinterpret_cast<uint64_t *>(ent + 8);
  }

  // uint64_t element_count = *sizePtr;
  // logger.info("元素总数 (element_count): {}", element_count);
  // logger.info("哈希掩码 (mask): {}", mask);
  // logger.info("Bucket {} 的条目数量: {}", h, entry_count);
  // 检查 head
  if (head) {
      BlockPos pos;
      std::memcpy(&pos, reinterpret_cast<void*>(head + 0x10), sizeof(pos));
      if (pos.x==key.x && pos.y==key.y && pos.z==key.z) {
          return head;
      }
  }
  return 0;
}

// 模拟 std::vector::erase(it) 
inline uint8_t* eraseVectorEntry(uint8_t*& start, uint8_t*& finish, uint8_t* it, size_t ENTRY = 32) {
    // assert(it >= start && it < finish);
    uint8_t* next = it + ENTRY;
    if (next < finish) {
        std::memmove(it, next, finish - next);
    }
    finish -= ENTRY;
    return it;
}

// 补清残留
static void __fastcall hooked_removeStaleRelationships(CircuitSceneGraph *scene) {
  // auto t0 = std::chrono::high_resolution_clock::now();
  // 不执行原版
  __try {
    // 拿到 pendingUpdates 哨兵
    uint64_t sentinel = *reinterpret_cast<uint64_t *>(reinterpret_cast<char *>(scene) + OFF_mPendingUpdates_relList);
    if (!sentinel) return;
    // 遍历每个更新项
    for (uint64_t cur = *reinterpret_cast<uint64_t *>(sentinel); cur && cur != sentinel; cur = *reinterpret_cast<uint64_t *>(cur)) 
    {
      BlockPos posUpdate;
      std::memcpy(&posUpdate, reinterpret_cast<void *>(cur + 0x10),sizeof(posUpdate));
      // logger.error("posUpdate.x{},posUpdate.y{},posUpdate.z{}",posUpdate.x, posUpdate.y, posUpdate.z);
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
          if (uptrPtr && uptrPtr->get()){
            auto rawComp = *reinterpret_cast<BaseCircuitComponent **>(reinterpret_cast<char *>(cur) + 0x20);
            uptrPtr->get()->removeSource(posUpdate, rawComp);
            // logger.error("清理成功chunkPos.x{},chunkPos.y{},chunkPos.z{}",chunkPos.x, chunkPos.y, chunkPos.z);
          }
        }
      } 
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    orig_removeStale(scene);
    logger.error("PowerAssociationMap 额外清理时发生异常，已跳过补充逻辑");
  }
  // 记录结束时间并计算差值
  // auto t1 = std::chrono::high_resolution_clock::now();
  // auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  // logger.info("hooked_removeStaleRelationships 耗时 {} ms", elapsed);
}

// // —— 钩子：测量 update 耗时 —— 
// static void __fastcall hooked_update(CircuitSceneGraph *scene, BlockSource *bs) {
//     auto t0 = std::chrono::high_resolution_clock::now();
//     // 调用原版 update
//     orig_update(scene, bs);
//     auto t1 = std::chrono::high_resolution_clock::now();
//     auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
//     logger.info("CircuitSceneGraph::update 耗时 {} ms", elapsed);
// }

// 安装钩子
bool installHook() {
  //  mce::Math::hash3
  computeHash3 = reinterpret_cast<Hash3Func>(dlsym_real("??$hash3@HHH@Math@mce@@SA_KAEBH00@Z"));
  if (!computeHash3) {
    logger.error("找不到 mce::Math::hash3 符号");
    return false;
  }

  void *addr =dlsym_real("?removeStaleRelationships@CircuitSceneGraph@@AEAAXXZ");
  if (!addr) {
    logger.error("找不到 removeStaleRelationships 符号");
    return false;
  }
  HookFunction(addr, reinterpret_cast<void **>(&orig_removeStale), reinterpret_cast<void *>(&hooked_removeStaleRelationships));
  
  // void *addr2 = dlsym_real("?update@CircuitSceneGraph@@QEAAXPEAVBlockSource@@@Z");
  // if (!addr2) {
  //     logger.error("找不到 CircuitSceneGraph::update 符号");
  //     return false;
  // }
  // HookFunction(addr2, reinterpret_cast<void**>(&orig_update), reinterpret_cast<void*>(hooked_update));
  // logger.info("Installed update hook @0x{:X}", (uintptr_t)addr2);

  // logger.info("PowerAssociationMapLeakFix 钩子安装完成 @0x{:X}",(uintptr_t)addr);
  return true;
}

} // namespace PowerAssociationMapLeakFix
