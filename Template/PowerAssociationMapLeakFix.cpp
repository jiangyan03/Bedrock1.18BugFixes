// PowerAssociationMapLeakFix.cpp
#include "PowerAssociationMapLeakFix.h"

#include <HookAPI.h>
#include <LoggerAPI.h>
#include <MC/BlockPos.hpp>
#include <MC/BaseCircuitComponent.hpp>
#include <MC/CircuitSceneGraph.hpp>

Logger PowerAssociationMapLeakFix::logger("PowerAssociationMapLeakFix");

// 保存原版函数指针
static void (__fastcall *orig_removeStale)(CircuitSceneGraph*);

namespace PowerAssociationMapLeakFix {

// BDS 1.18.2 中 CircuitSceneGraph 内部偏移
static constexpr size_t OFF_mAllComponents          = 0x00;   // std::unordered_map<BlockPos, unique_ptr<BaseCircuitComponent>>
static constexpr size_t OFF_mPowerAssociationMap    = 0x98;   // std::unordered_map<BlockPos, CircuitComponentList>
static constexpr size_t OFF_mPendingUpdates_relList = 0x120;  // pendingUpdates 链表哨兵

// MSVC STL unordered_map 内部 _Hash 偏移
static constexpr ptrdiff_t OFF_mask    = 0x30;
static constexpr ptrdiff_t OFF_buckets = 0x18;
static constexpr ptrdiff_t OFF_end     = 0x08;

// CircuitComponentList 中每条 entry 大小
static constexpr size_t ENTRY = 32;

// 哈希函数签名（mce::Math::hash3）
using Hash3Func = uint64_t(*)(const int&, const int&, const int&);
static Hash3Func computeHash3 = nullptr;

// O(1) 获取 unordered_map 的近似大小（桶数 = mask + 1）
static size_t getHashMapSizeApprox(CircuitSceneGraph* scene, size_t tableOffset) {
    auto base = reinterpret_cast<uint8_t*>(scene) + tableOffset;
    auto maskPtr = reinterpret_cast<uint64_t*>(base + OFF_mask);
    if (!maskPtr) return 0;
    return (*maskPtr) + 1;
}

// 从内存读取 unordered_map 条目
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
    size_t idx = static_cast<size_t>(h) * 2;
    uint64_t* bucket = buckets + idx;
    if (!bucket[0] || bucket == endNode) return 0;

    // 从尾遍历链表
    uint64_t ent   = bucket[1];
    uint64_t start = bucket[0];
    while (ent && ent != start) {
        BlockPos np;
        std::memcpy(&np, reinterpret_cast<void*>(ent + 0x10), sizeof(np));
        if (np.x == key.x && np.y == key.y && np.z == key.z) return ent;
        ent = *reinterpret_cast<uint64_t*>(ent + 8);
    }
    return 0;
}

// 钩子：先调用原版，再补清漏网之鱼
static void __fastcall hooked_removeStaleRelationships(CircuitSceneGraph* scene) {
    // 1) 执行原版逻辑
    orig_removeStale(scene);

    // 2) 额外清理：遍历所有 pendingUpdates，针对每个 posUpdate 手动清空 PowerAssociationMap 中的残留
    __try {
        uint64_t sentinel = *reinterpret_cast<uint64_t*>(
            reinterpret_cast<char*>(scene) + OFF_mPendingUpdates_relList
        );
        if (!sentinel) return;
            // **O(1) 打印 mPowerAssociationMap 大小**

        // scene + OFF_mPowerAssociationMap 就是 map 对象的地址
        // size_t approxSize1 = getHashMapSizeApprox(scene, OFF_mPowerAssociationMap);
        // logger.info("PowerAssociationMap 当前桶数(近似容器大小): {}", approxSize1);
        // size_t approxSize2 = getHashMapSizeApprox(scene, OFF_mAllComponents);
        // logger.info("AllComponents 当前桶数(近似容器大小): {}", approxSize2);
        for (uint64_t cur = *reinterpret_cast<uint64_t*>(sentinel);
             cur && cur != sentinel;
             cur = *reinterpret_cast<uint64_t*>(cur))
        {
            // 取更新项位置与原始组件指针
            BlockPos posUpdate;
            std::memcpy(&posUpdate, reinterpret_cast<void*>(cur + 0x10), sizeof(posUpdate));
            auto rawComp = *reinterpret_cast<BaseCircuitComponent**>(reinterpret_cast<char*>(cur) + 0x20);
            if (!rawComp) continue;

            // 在 PowerAssociationMap 中查找
            uint64_t entPower = lookupHashMap(scene, OFF_mPowerAssociationMap, posUpdate);
            if (!entPower) continue;

            // 拿到 vector 开始/结束
            char* beginPtr = *reinterpret_cast<char**>(entPower + 0x20);
            char* endPtr   = *reinterpret_cast<char**>(entPower + 0x28);
            size_t len     = (endPtr - beginPtr) / ENTRY;

            // 逐条 erase：尾部替换 + 缩短
            while (len > 0) {
                // 读 chunkPos
                BlockPos chunk;
                std::memcpy(&chunk, beginPtr + (len-1)*ENTRY, sizeof(chunk));

                // removeSource
                if (uint64_t entAll = lookupHashMap(scene, OFF_mAllComponents, chunk)) {
                    auto uptrPtr = reinterpret_cast<std::unique_ptr<BaseCircuitComponent>*>(entAll + 0x20);
                    if (uptrPtr && uptrPtr->get()) {
                        uptrPtr->get()->removeSource(posUpdate, rawComp);
                    }
                }

                // 用最后一条覆盖当前位置
                if (len > 1) {
                    std::memcpy(beginPtr + (len-1)*ENTRY, beginPtr + (len-2)*ENTRY, ENTRY);
                }
                --len;
                endPtr = beginPtr + len*ENTRY;
                *reinterpret_cast<char**>(entPower + 0x28) = endPtr;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logger.error("PowerAssociationMap 额外清理时发生异常，已跳过补充逻辑");
    }
}

// 安装钩子入口
bool installHook() {
    // 解析真实 hash3 符号
    computeHash3 = reinterpret_cast<Hash3Func>(
        dlsym_real("??$hash3@HHH@Math@mce@@SA_KAEBH00@Z")
    );

    // hook removeStaleRelationships
    void* addr = dlsym_real("?removeStaleRelationships@CircuitSceneGraph@@AEAAXXZ");
    if (!addr) {
        logger.error("找不到 removeStaleRelationships 符号");
        return false;
    }
    HookFunction(
        addr,
        reinterpret_cast<void**>(&orig_removeStale),
        reinterpret_cast<void*>(&hooked_removeStaleRelationships)
    );
    logger.info("PowerAssociationMapLeakFix 钩子安装完成 @0x{:X}", (uintptr_t)addr);
    return true;
}

} // namespace PowerAssociationMapLeakFix
