// PowerAssociationMapLeakFix.cpp
#include "PowerAssociationMapLeakFix.h"

#include <HookAPI.h>
#include <LoggerAPI.h>
#include <MC/BlockPos.hpp>
#include <MC/BaseCircuitComponent.hpp>
#include <MC/CircuitSceneGraph.hpp>

Logger PowerAssociationMapLeakFix::logger("PowerAssociationMapLeakFix");

static void (__fastcall *orig_removeStale)(CircuitSceneGraph*); // 可忽略不调用

namespace PowerAssociationMapLeakFix {

// ========== 假设说明（适用于 BDS 1.18.2） ==========
// 假设 A: CircuitSceneGraph 内部的 mAllComponents/mPowerAssociationMap/mPendingUpdates 的偏移如下：
static constexpr size_t OFF_mAllComponents       = 0x00;//40?00?
static constexpr size_t OFF_mPowerAssociationMap = 0x98;//58?0xB0?0x98?
static constexpr size_t OFF_mPendingUpdates      = 0x198;
static constexpr size_t OFF_relList = 0x120;

// 假设 B: 三个 unordered_map 的底层使用 MSVC STL，且其内部 _Hash 表结构一致：
// (unordered_map -> _Hash -> _List/_Vector + 桶信息)
// mask/buckets/endNode 均位于 tableOffset 偏移之后固定距离
static constexpr ptrdiff_t OFF_mask    = 0x30; // hash._Mask   0x30?0x18?
static constexpr ptrdiff_t OFF_buckets = 0x18; // hash._Buckets 0x38?0x0?
static constexpr ptrdiff_t OFF_end     = 0x08; // hash._EndNode 0x18

// 假设 C: PowerAssociationMap 的值类型为 struct { BlockPos, ..., ..., ..., ..., ..., ..., ... }
// size 为 32 字节，每条映射 entry 占 32 字节
constexpr size_t ENTRY = 32;

// === Hash3函数接口 ===
using Hash3Func = uint64_t(*)(const int& x, const int& y, const int& z);
// ========== 通用查找表函数 ==========
//   参数 tableOffset: 哈希表在 CircuitSceneGraph 中的偏移
//   返回值: 如果找到 entry 节点，则返回其地址；否则返回 0
uint64_t default_hash3(const int& x, const int& y, const int& z) {
    constexpr uint64_t FNV_offset_basis = 0xcbf29ce484222325;
    constexpr uint64_t FNV_prime        = 0x100000001b3;
    constexpr uint32_t GOLDEN_RATIO     = 0x9E3779B9;

    auto hash_int = [&](const int& val, uint64_t seed) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
        for (int i = 0; i < 4; ++i) {
            seed ^= p[i];
            seed *= FNV_prime;
        }
        return seed;
    };

    // 第一个整数
    uint64_t h = hash_int(x, FNV_offset_basis);
    // 第二个整数
    h = hash_int(y, h + GOLDEN_RATIO);
    // 第三个整数
    h = hash_int(z, h + GOLDEN_RATIO);

    return h;
}

// 当前使用的哈希函数，默认指向 default_hash3
static Hash3Func computeHash3 = default_hash3;
// 设置为真实符号函数（可选）
bool setHash3FromSymbol(const char* symbolName) {
    void* addr = dlsym_real(symbolName);
    if (!addr) {
        logger.error("无法定位哈希函数符号: {}", symbolName);
        return false;
    }
    computeHash3 = reinterpret_cast<Hash3Func>(addr);
    logger.info("Hash3 函数已替换为外部符号: {} @ 0x{:X}", symbolName, (uintptr_t)addr);
    return true;
}

static uint64_t lookupHashMap(
    CircuitSceneGraph* scene,
    size_t             tableOffset,
    const BlockPos&    key
) {
    if (!scene) {
        logger.error("lookupHashMap: scene is null");
        return 0;
    }

    // [1] 计算哈希值（使用 mce_hash3模拟内部的??$hash3@HHH@Math@mce@@SA_KAEBH00@Z proc near）
    int buf[3] = { key.x, key.y, key.z };
    // logger.error("Coordinates: x={}, y={}, z={}", buf[0], buf[1], buf[2]);
    uint64_t h = computeHash3(key.x, key.y, key.z);
    // uint64_t h = mce_hash3(reinterpret_cast<const char*>(buf), sizeof(buf));

    // [2] 获取哈希表结构（关键：重新校准偏移量）
    // char* hashTable = reinterpret_cast<char*>(scene) + tableOffset;
    // uint64_t* maskPtr = reinterpret_cast<uint64_t*>(hashTable + OFF_mask);
    // uint64_t** buckets = reinterpret_cast<uint64_t**>(hashTable + OFF_buckets);
    // uint64_t* endNode = reinterpret_cast<uint64_t*>(hashTable + OFF_end);
    auto base        = reinterpret_cast<uint8_t*>(scene) + tableOffset;
    auto maskPtr     = reinterpret_cast<uint64_t*>(base + OFF_mask);
    auto bucketsBase = reinterpret_cast<void**>(base + OFF_buckets);  // 先读地址
    auto buckets     = reinterpret_cast<uint64_t**>(*bucketsBase);      // 再用作桶数组
    auto endNode     = reinterpret_cast<uint64_t*>(base + OFF_end);

    // —— 新增：打印三指针地址与内容 —— 
    // logger.error(
    //   "lookupHashMap@0x{:X}: maskPtr=0x{:X} (*=0x{:X}), buckets=0x{:X}, endNode=0x{:X}",
    //   (uintptr_t)base,
    //   (uintptr_t)maskPtr,
    //   maskPtr ? *maskPtr : 0xDEADBEEF,
    //   (uintptr_t)buckets,
    //   (uintptr_t)endNode
    // );
    // —— 结束打印 —— 

    // [3] 严格检查指针有效性
    if (!maskPtr || !buckets || !endNode) {
        logger.error("Invalid hash table pointers (tableOffset=0x{:X})", tableOffset);
        return 0;
    }

    __try {
        // [4] 读取 mask 并校验合法性
        uint64_t mask = *maskPtr;
        if (mask == 0 || mask > 0xFFFF) {
            logger.error("Invalid mask: 0x{:X} (tableOffset=0x{:X})", mask, tableOffset);
            return 0;
        }

        // [5] 计算桶索引（每个桶占两个指针：head/tail）
        // size_t bucketIndex = (h & mask) * 2;
        // if (bucketIndex >= (mask + 1) * 2) {
        //     logger.error("Bucket index overflow (idx={}, mask=0x{:X})", bucketIndex, mask);
        //     return 0;
        // }
        size_t bucketCount = (mask + 1) * 2;
        size_t bucketIndex = (h & mask) * 2;
        // logger.error(" mask=0x{:X}, bucketCount={}, bucketIndex={}",
        //              mask, bucketCount, bucketIndex);
        if (bucketIndex >= bucketCount) {
            logger.error("Bucket index overflow (idx={}, mask=0x{:X})", bucketIndex, mask);
            return 0;
        }

        // [6] 获取桶指针并检查空桶
        uint64_t* bucket = buckets[bucketIndex];
        if (!bucket || bucket == endNode) {
            return 0; // 空桶
        }

        // [7] 从尾节点开始遍历链表（反编译代码逻辑）
        uint64_t ent = bucket[1]; // 尾节点
        uint64_t start = bucket[0]; // 头节点
        while (ent != start) {
            // [7.1] 提取 BlockPos（偏移 +0x10）
            BlockPos nodePos;
            std::memcpy(&nodePos, reinterpret_cast<void*>(ent + 0x10), sizeof(nodePos));
            // [7.2] 检查是否匹配
            if (nodePos.x == key.x && nodePos.y == key.y && nodePos.z == key.z) {
                logger.info("got x={}, y={}, z={}",  nodePos.x, nodePos.y, nodePos.z);
                return ent;
            }
            // [7.3] 移动到下一个节点（next 指针偏移 +0x8）
            ent = *reinterpret_cast<uint64_t*>(ent + 8);
        }
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logger.error(
            "Memory access exception (tableOffset=0x{:X}, mask=0x{:X})", 
            tableOffset, maskPtr ? *maskPtr : 0
        );
        return 0;
    }
}


// ========== Hook 实现 ==========
// 不调用原版 removeStaleRelationships
static void __fastcall hooked_removeStaleRelationships(CircuitSceneGraph* scene) {
    __try {
        // 1. 取哨兵地址
        uint64_t sentinel = *reinterpret_cast<uint64_t*>((char*)scene + OFF_relList);
        if (!sentinel) return;

        // 2. 从 sentinel->next 开始遍历
        uint64_t head = sentinel;
        uint64_t cur  = *reinterpret_cast<uint64_t*>(sentinel);
        while (cur && cur != head) {
            // 先备份 next
            uint64_t next = *reinterpret_cast<uint64_t*>(cur);

            // --- 你的节点处理逻辑 ---
            // 3. 读 BlockPos
            int px = *reinterpret_cast<int*>((char*)cur + 0x30);
            int py = *reinterpret_cast<int*>((char*)cur + 0x34);
            int pz = *reinterpret_cast<int*>((char*)cur + 0x38);
            BlockPos posUpd{ px, py, pz };
            // logger.error("Parsed BlockPos from relList: px={}, py={}, pz={}", px, py, pz);

            // 4. 取组件指针
            auto rawComp = *reinterpret_cast<BaseCircuitComponent**>((char*)cur + 0x20);
            if (rawComp) {
                // 5. 查 powerAssociationMap
                uint64_t entPower = lookupHashMap(scene, OFF_mPowerAssociationMap, posUpd);
                if (entPower) {
                    char* start  = *reinterpret_cast<char**>(entPower + 0x20);
                    char* finish = *reinterpret_cast<char**>(entPower + 0x28);
                    // 6. 遍历关系列表
                    for (char* it = start; it < finish; /* 迭代里自己推进 */) {
                        // 6.1 读 chunk
                        int cx = *reinterpret_cast<int*>(it + 0);
                        int cy = *reinterpret_cast<int*>(it + 4);
                        int cz = *reinterpret_cast<int*>(it + 8);
                        BlockPos chunk{ cx, cy, cz };
                        // logger.error("Parsed BlockPos from relList: cx={}, cy={}, cz={}", cx, cy, cz);

                        // 6.2 调原版 removeSource
                        if (auto entAll = lookupHashMap(scene, OFF_mAllComponents, chunk)) {
                            auto uptr = reinterpret_cast<std::unique_ptr<BaseCircuitComponent>*>(entAll + 0x20);
                            if (auto* comp = uptr->get()) {
                                comp->removeSource(posUpd, rawComp);
                            }
                        }
                        // 6.3 memmove 删除 entry，更新 finish
                        std::memmove(it, it + ENTRY, finish - it - ENTRY);
                        finish -= ENTRY;
                        *reinterpret_cast<char**>(entPower + 0x28) = finish;
                        // (注意：这里不要 advance it——因为 memmove 之后它已经指向下一个元素了)
                    }
                }
            }
            // --- end 节点处理 ---

            // 7. 推进到下一个节点
            cur = next;
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        orig_removeStale(scene);
        logger.error("hooked_removeStaleRelationships: 捕获异常！");
    }
}

// ========== 安装钩子函数 ==========
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
            logger.error("钩子安装失败，错误码: {}", hookResult);
            return false;
        }

        logger.info("钩子安装成功，地址: 0x{:X}", (uintptr_t)addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logger.fatal("安装钩子时触发内存访问异常！");
        return false;
    }
}

} // namespace PowerAssociationMapLeakFix
