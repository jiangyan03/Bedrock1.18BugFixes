// PowerAssociationMapLeakFix.cpp
#include "PowerAssociationMapLeakFix.h"

#include <HookAPI.h>
#include <LoggerAPI.h>
#include <MC/BlockPos.hpp>
#include <MC/BaseCircuitComponent.hpp>
#include <MC/CircuitSceneGraph.hpp>

Logger PowerAssociationMapLeakFix::logger("PowerAssociationMapLeakFix");

// 原版函数指针占位
static void (__fastcall *orig_removeStale)(CircuitSceneGraph*);

namespace PowerAssociationMapLeakFix {

// CircuitSceneGraph 内偏移
static constexpr size_t OFF_mAllComponents          = 0x00;
static constexpr size_t OFF_mPowerAssociationMap    = 0x98;
static constexpr size_t OFF_mPendingUpdates_relList = 0x120;

// MSVC unordered_map 内部偏移
static constexpr ptrdiff_t OFF_mask    = 0x30;
static constexpr ptrdiff_t OFF_Maxidx  = 0x28;
static constexpr ptrdiff_t _Equal_func = 0x20;
static constexpr ptrdiff_t OFF_buckets = 0x18;
static constexpr ptrdiff_t OFF_size    = 0x10;
static constexpr ptrdiff_t OFF_end     = 0x08;

// CircuitComponentList entry 大小
static constexpr size_t ENTRY = 32;

// hash3 签名
using Hash3Func = uint64_t(*)(const int&, const int&, const int&);
static Hash3Func computeHash3 = nullptr;
// MSVC 两参数 sized delete
using DeleteSizedFn = void(__cdecl*)(void* ptr, size_t sz);
static DeleteSizedFn real_delete_sized = nullptr;

// 从内存中快速查条目（不变）
static uint64_t lookupHashMap(CircuitSceneGraph* scene, size_t tableOffset, const BlockPos& key) {
    auto base       = reinterpret_cast<uint8_t*>(scene) + tableOffset;
    auto maskPtr    = reinterpret_cast<uint64_t*>(base + OFF_mask);
    auto bucketsPtr = reinterpret_cast<void**>(base + OFF_buckets);
    auto endNode    = reinterpret_cast<uint64_t*>(base + OFF_end);
    if (!maskPtr || !bucketsPtr || !endNode) return 0;
    uint64_t mask = *maskPtr;
    if (!computeHash3 || mask == 0) return 0;

    uint64_t h = computeHash3(key.x, key.y, key.z) & mask;
    auto   buckets = reinterpret_cast<uint64_t*>(*bucketsPtr);
    auto*  bucket  = buckets + h*2;
    if (!bucket[0] || bucket == endNode) return 0;

    // 从 tail 向 head 搜
    uint64_t ent = bucket[1], head = bucket[0];
    while (ent && ent != head) {
        BlockPos np;
        std::memcpy(&np, reinterpret_cast<void*>(ent + 0x10), sizeof(np));
        if (np.x==key.x && np.y==key.y && np.z==key.z) return ent;
        ent = *reinterpret_cast<uint64_t*>(ent + 8);
    }
    return 0;
}

// 完整的双向链表 erase，实现真正从桶链里摘除 node
static void eraseHashMapEntry(CircuitSceneGraph* scene, size_t tableOffset, const BlockPos& key) {
    auto* base       = reinterpret_cast<uint8_t*>(scene) + tableOffset;
    auto* maskPtr    = reinterpret_cast<uint64_t*>(base + OFF_mask);
    auto* bucketsPtr = reinterpret_cast<void**>(base + OFF_buckets);
    uint64_t  endNode = *reinterpret_cast<uint64_t*>(base + OFF_end);

    // for (int off = 0; off < 0x100; off += 2) {
    // auto *val = reinterpret_cast<uint64_t*>(base + off);
    // if (*val > 0 && *val < 100000) {
    //     logger.error("Offset {:02X} = {}", off, *val);
    //     }
    // }
    auto* sizePtr    = reinterpret_cast<uint64_t*>(base + OFF_size);
    
    if (!maskPtr || !bucketsPtr || !sizePtr) return;

    uint64_t mask = *maskPtr;
    if (!computeHash3 || mask == 0) return;

    // 1) 计算 bucket 索引
    uint64_t h      = computeHash3(key.x, key.y, key.z) & mask;

    auto*  buckets = reinterpret_cast<uint64_t*>(*bucketsPtr);
    uint64_t* bucket = buckets + h*2;

    // head/tail 都是真正的节点指针
    uint64_t head = bucket[0];
    uint64_t tail = bucket[1];

    // 2) 遍历循环链表，从 tail 出发，一定能回到 tail 或者到达 endNode
    uint64_t node = tail;
    // logger.error("keyCoordinates: x={}, y={}, z={}, sizePtr={}", key.x, key.y, key.z, *sizePtr);
    while (node && node != endNode) {
        BlockPos np;
        std::memcpy(&np, reinterpret_cast<void*>(node + 0x10), sizeof(np));
        if (np.x == key.x && np.y == key.y && np.z == key.z) {
            // 找到要删的节点，修正双链表指针
            uint64_t prev = *reinterpret_cast<uint64_t*>(node + 0);
            uint64_t next = *reinterpret_cast<uint64_t*>(node + 8);

            // 更新 bucket 的 head/tail 或前后节点
            if (tail == node)       bucket[1] = prev;
            else                    *reinterpret_cast<uint64_t*>(prev + 8) = next;

            if (head == node)       bucket[0] = next;
            else                    *reinterpret_cast<uint64_t*>(next + 0) = prev;

            // 更新 size
            --*sizePtr;
            // 调用 MSVC sized delete，第二个参数就是反汇编里看到的 0x28
            real_delete_sized(reinterpret_cast<void*>(node), 0x28);
            logger.error("npCoordinates: x={}, y={}, z={}, sizePtr={}", np.x, np.y, np.z, *sizePtr);
            return;
        }
        node = *reinterpret_cast<uint64_t*>(node + 8);
    }
}

// 钩子：先跑原版，再补清残留
static void __fastcall hooked_removeStaleRelationships(CircuitSceneGraph* scene) {
    // 执行原版
    __try {
        // 拿到 pendingUpdates 哨兵
        uint64_t sentinel = *reinterpret_cast<uint64_t*>(
            reinterpret_cast<char*>(scene) + OFF_mPendingUpdates_relList
        );
        if (!sentinel) return;
        // 遍历每个更新项
        for (uint64_t cur = *reinterpret_cast<uint64_t*>(sentinel);
             cur && cur != sentinel;
             cur = *reinterpret_cast<uint64_t*>(cur))
        {
            BlockPos posUpdate;
            std::memcpy(&posUpdate, reinterpret_cast<void*>(cur + 0x10), sizeof(posUpdate));
            auto rawComp = *reinterpret_cast<BaseCircuitComponent**>(reinterpret_cast<char*>(cur) + 0x20);
            if (!rawComp) continue;

            // 在 PowerAssociationMap 中找那个 vector 结构
            uint64_t entPower = lookupHashMap(scene, OFF_mPowerAssociationMap, posUpdate);
            if (!entPower) continue;

            // 清空它的 mComponents
            char* beginPtr = *reinterpret_cast<char**>(entPower + 0x20);
            char* endPtr   = *reinterpret_cast<char**>(entPower + 0x28);
            size_t len     = (endPtr - beginPtr) / ENTRY;
            // 如果已经清空，连 map 的这条 key 一并删掉
            while (len > 0) {
                // 读出 chunkPos，调用 removeSource
                BlockPos chunk;
                std::memcpy(&chunk, beginPtr + (len-1)*ENTRY, sizeof(chunk));
                if (uint64_t entAll = lookupHashMap(scene, OFF_mAllComponents, chunk)) {
                    auto uptrPtr = reinterpret_cast<std::unique_ptr<BaseCircuitComponent>*>(entAll + 0x20);
                    if (uptrPtr && uptrPtr->get()) {
                        uptrPtr->get()->removeSource(posUpdate, rawComp);
                    }
                }
                // 用最后一条覆盖，再缩短
                if (len>1) {
                    std::memcpy(beginPtr + (len-1)*ENTRY, beginPtr + (len-2)*ENTRY, ENTRY);
                }
                --len;
                endPtr = beginPtr + len*ENTRY;
                *reinterpret_cast<char**>(entPower + 0x28) = endPtr;
                // __try{eraseHashMapEntry(scene, OFF_mPowerAssociationMap, posUpdate);}
                // __except (EXCEPTION_EXECUTE_HANDLER) {
                //     logger.error("eraseHashMapEntry错误");
                // }
            }
            // 如果已经清空，连 map 的这条 key 一并删掉
            if(endPtr == beginPtr){eraseHashMapEntry(scene, OFF_mPowerAssociationMap, posUpdate);}
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        orig_removeStale(scene);
        logger.error("PowerAssociationMap 额外清理时发生异常，已跳过补充逻辑");
    }
}

// 安装钩子
bool installHook() {
    // 拿到 mce::Math::hash3
    computeHash3 = reinterpret_cast<Hash3Func>(
        dlsym_real("??$hash3@HHH@Math@mce@@SA_KAEBH00@Z")
    );
    // 拿到 sized delete
    real_delete_sized = reinterpret_cast<DeleteSizedFn>(
        dlsym_real("??3@YAXPEAX_K@Z")
    );
    void* addr = dlsym_real("?removeStaleRelationships@CircuitSceneGraph@@AEAAXXZ");
    if (!addr) {
        logger.error("找不到 removeStaleRelationships 符号");
        return false;
    }
    HookFunction(addr, reinterpret_cast<void**>(&orig_removeStale),
                      reinterpret_cast<void*>(&hooked_removeStaleRelationships));
    logger.info("PowerAssociationMapLeakFix 钩子安装完成 @0x{:X}", (uintptr_t)addr);
    return true;
}

} // namespace PowerAssociationMapLeakFix
