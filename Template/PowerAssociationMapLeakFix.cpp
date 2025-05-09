// PowerAssociationMapLeakFix.cpp
#include "PowerAssociationMapLeakFix.h"
#include <HookAPI.h>
#include <LoggerAPI.h>
#include <MC/BlockPos.hpp>
#include <MC/BaseCircuitComponent.hpp>
#include <MC/CircuitSceneGraph.hpp>
#include <Utils/Hash.h>    // do_hash
#include <cstring>         // std::memcpy, std::memmove
Logger PowerAssociationMapLeakFix::logger("PowerAssociationMapLeakFix");
static void (__fastcall *orig_removeStale)(CircuitSceneGraph*); // 可忽略不调用

namespace PowerAssociationMapLeakFix {
// ——— 偏移（请根据你的反编译结果微调） ———
static constexpr size_t OFF_mAllComponents       = 0x40;   // this + 0x40: mAllComponents
static constexpr size_t OFF_mPowerAssociationMap = 0x58;   // this + 0x58: mPowerAssociationMap
static constexpr size_t OFF_mPendingUpdates      = 0x118;  // this + 0x118: mPendingUpdates

// 每张 unordered_map 内部的 _Hash 桶结构相对表头的偏移（MSVC STL）
// 这里假设三张表的内部布局相同，仅偏移不同
static constexpr ptrdiff_t OFF_mask    = 0xC8;  // bucketMask:   hash._Mask
static constexpr ptrdiff_t OFF_buckets = 0xB0;  // buckets base: hash._Buckets
static constexpr ptrdiff_t OFF_end     = 0xA0;  // end sentinel: hash._EndNode

// ——— 通用查表：返回“条目节点”地址（或 0） ———
//   tableOffset: 三张表在 this 结构体内的起始偏移
static uint64_t lookupHashMap(
    CircuitSceneGraph* scene,
    size_t              tableOffset,
    const BlockPos&     key
) {
    if (!scene) {
        logger.error("lookupHashMap空指针");
        return 0;
    } // 空指针检查
    // 1) 计算三维坐标哈希
    uint32_t buf[3] = {
        static_cast<uint32_t>(key.x),
        static_cast<uint32_t>(key.y),
        static_cast<uint32_t>(key.z)
    };
    uint64_t h = do_hash(reinterpret_cast<char*>(buf), sizeof(buf));
    
    // 2) 拿到 mask / buckets / end
    auto* maskPtr = reinterpret_cast<uint64_t*>(
        reinterpret_cast<char*>(scene) + tableOffset + OFF_mask
    );
    auto** buckets = reinterpret_cast<uint64_t**>(
        reinterpret_cast<char*>(scene) + tableOffset + OFF_buckets
    );
    auto*  endNode = reinterpret_cast<uint64_t*>(
        reinterpret_cast<char*>(scene) + tableOffset + OFF_end
    );
    // ✅ 立即打印这几个指针以检查是否有效
    logger.info("tableOffset=0x{:X} => maskPtr={}, buckets={}, endNode={}", tableOffset, (void*)maskPtr, (void*)buckets, (void*)endNode);

    __try {
        uint64_t mask = *maskPtr;
        if (mask > 0xFFFF) {
            logger.error("异常 mask 值: 0x{:X}", mask);
            return 0; // 防止读野指针
        } // 检查关键指针

        logger.info("mask=0x{:X}, buckets[0]={}, endNode=0x{:X}",
            mask, (void*)buckets[0], (uint64_t)endNode);

        // 3) 取尾指针判断“空桶”
        size_t   idx  = (h & mask) * 2;    // 每桶两个指针：head/tail
        uint64_t ent = buckets[idx+1][0];
        if (!buckets[0] || !buckets[1]) {
            logger.error("空桶：buckets[0]={}, buckets[1]={}", (void*)buckets[0], (void*)buckets[1]);
            return 0;
        }
        
        // 4) 遍历链头直到找到匹配
        uint64_t start = buckets[idx][0];
        while (true) {
            int32_t x = *reinterpret_cast<int32_t*>(ent + 0x10);
            int32_t y = *reinterpret_cast<int32_t*>(ent + 0x14);
            int32_t z = *reinterpret_cast<int32_t*>(ent + 0x18);
            if (x==key.x && y==key.y && z==key.z) break;
            if (ent==start) { ent = 0; break; }
            ent = *reinterpret_cast<uint64_t*>(ent + 8);
        }
        logger.info("查找 HashMap (offset=0x{:X}): key=({}, {}, {}), mask=0x{:X}, idx={}", tableOffset, key.x, key.y, key.z, mask, idx);
        if (!ent) ent = reinterpret_cast<uint64_t>(endNode);
        return ent == reinterpret_cast<uint64_t>(endNode) ? 0 : ent;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logger.error("访问哈希表结构时触发异常！");
        return 0;
    }
}

// ——— Hook 实现（不调用原版） ———
static void __fastcall hooked_removeStaleRelationships(CircuitSceneGraph* scene) {
    __try {
        if (!scene) {orig_removeStale(scene); return;}
        // 1) 遍历待更新队列
        uint64_t* headPtr = reinterpret_cast<uint64_t*>(
            reinterpret_cast<char*>(scene) + OFF_mPendingUpdates + 0x8
        );
        if (!headPtr || !*headPtr) return; // 检查链表头有效性

        uint64_t head = *headPtr;
        uint64_t cur = head;
        while (true) {
            cur = *reinterpret_cast<uint64_t*>(cur);
            if (!cur || cur == head) break;
            uint64_t node = cur;
            if (!node) continue;

            // 1.1 提取位置和组件
            BlockPos posUpd;
            // std::memcpy(&posUpd, node + 0x10, sizeof(posUpd));
            std::memcpy(&posUpd, reinterpret_cast<void*>(node + 0x10), sizeof(posUpd));
            // auto* rawComp = *reinterpret_cast<BaseCircuitComponent**>(node + 0x20);
            BaseCircuitComponent* rawComp = *reinterpret_cast<BaseCircuitComponent**>(reinterpret_cast<void*>(node + 0x20));
            if (!rawComp) continue;

            // 2) 清理关联映射
            uint64_t entPower = lookupHashMap(scene, OFF_mPowerAssociationMap, posUpd);
            if (entPower) {
                char* start = *reinterpret_cast<char**>(entPower + 0x20);
                char* finish = *reinterpret_cast<char**>(entPower + 0x28);
                constexpr size_t ENTRY = 32;

                for (char* it = start; it < finish; ) {
                    BlockPos chunk;
                    std::memcpy(&chunk, it, sizeof(chunk));

                    // 2.1 查找组件并移除关联
                    if (auto entAll = lookupHashMap(scene, OFF_mAllComponents, chunk)) {
                        auto uptr = reinterpret_cast<std::unique_ptr<BaseCircuitComponent>*>(entAll + 0x20);
                        if (auto* comp = uptr->get()) {
                            comp->removeSource(posUpd, rawComp);
                        }
                    }

                    // 2.2 内存搬移
                    std::memmove(it, it + ENTRY, finish - it - ENTRY);
                    finish -= ENTRY;
                    *reinterpret_cast<char**>(entPower + 0x28) = finish;
                }
            }
        }
    } 
    __except(EXCEPTION_EXECUTE_HANDLER) {
        orig_removeStale(scene);
        logger.error("hooked_removeStaleRelationships: 捕获异常！");
        return;
    }
}

// 安装 Hook
bool installHook() {
    logger.info("?removeStaleRelationships@CircuitSceneGraph@@AEAAXXZ");
    constexpr auto sym = "?removeStaleRelationships@CircuitSceneGraph@@AEAAXXZ";
    __try {
        void* addr = dlsym_real(sym);
        if (!addr) {
            logger.error("符号未找到: {}", sym);
            return false;
        }
        logger.info("符号地址: 0x{:X}", (uintptr_t)addr);

        int hookResult = HookFunction(
            addr,
            reinterpret_cast<void**>(&orig_removeStale),
            reinterpret_cast<void*>(&hooked_removeStaleRelationships)
        );
        if (hookResult != 0) {
            logger.error(" 钩子安装失败，错误码: {}", hookResult);
            return false;
        }

        logger.info("钩子安装成功，地址: 0x{:X}", (uintptr_t)addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logger.fatal("安装钩子时触发内存访问异常！");
        return false;
    }
}

} // namespace PowerAssociationMapLeakFix
