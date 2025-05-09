// PowerAssociationMapLeakFix.cpp
#include "PowerAssociationMapLeakFix.h"
#include <HookAPI.h>
#include <MC/BlockPos.hpp>
#include <MC/CircuitSceneGraph.hpp>

//
// —— POD 结构，只保证内存布局与真实类型二进制兼容 ——
//

// 对应 std::vector<BlockPos> / std::vector<PendingEntry> 之类的起止指针布局
template<typename T>
struct VectorPOD {
    T*    _M_start;
    T*    _M_finish;
    T*    _M_end_of_storage;
};

// 对应 CircuitComponentList: start / finish / end_of_storage
struct CircuitComponentListPOD {
    void* _M_start;
    void* _M_finish;
    void* _M_end_of_storage;
};

// 对应 CircuitSceneGraph::PendingEntry 链表节点布局
struct PendingEntryPOD {
    uint64_t _next;
    uint64_t _prev;
    BlockPos mPos;
    void*    mRawComponentPtr;
};

// --------------------------------------------------
// 内存偏移（需与 BDS 1.18.2 保持一致！）
// --------------------------------------------------
static constexpr size_t OFF_mPowerAssociationMap = 0x58; // this+0x58 是第二个 map
//                                       （若你起点按 0x40 对齐，请改为 0x98）

// --------------------------------------------------
// 哈希与比较策略（与引擎一致）
// --------------------------------------------------
struct BlockPosHash {
    size_t operator()(BlockPos const& pos) const noexcept {
        // 参考 mce::Math::hash3<int,int,int>
        uint64_t k1 = static_cast<uint64_t>(pos.x);
        uint64_t k2 = static_cast<uint64_t>(pos.y) << 21;
        uint64_t k3 = static_cast<uint64_t>(pos.z) << 42;
        // 简版：三者异或再折散
        uint64_t h = k1 ^ k2 ^ k3;
        // 高位混沌
        h ^= (h >> 33);
        h *= 0xff51afd7ed558ccdULL;
        h ^= (h >> 33);
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= (h >> 33);
        return static_cast<size_t>(h);
    }
};

namespace PowerAssociationMapLeakFix {

// --------------------------------------------------
// 使用 POD 类型但配合正确哈希和 equal_to
// --------------------------------------------------
using PowerMapType = std::unordered_map<
    BlockPos,
    CircuitComponentListPOD,
    BlockPosHash,
    std::equal_to<BlockPos>
>;

// 原始函数指针
using FnRemoveStale = void (__fastcall *)(CircuitSceneGraph *);
static FnRemoveStale orig_removeStale = nullptr;

// --------------------------------------------------
// Hook 实现：先跑原版，再删除“空”的 power-list
// --------------------------------------------------
static void __fastcall hooked_removeStaleRelationships(CircuitSceneGraph* self) {
    // 1) 调用引擎原始逻辑
    orig_removeStale(self);

    // 2) 拿到 mPowerAssociationMap 的指针
    auto* powerMap = reinterpret_cast<PowerMapType*>(
        reinterpret_cast<char*>(self) + OFF_mPowerAssociationMap
    );

    // 3) 收集所有“空列表”的 key
    std::vector<BlockPos> toErase;
    toErase.reserve(16);
    for (auto const& kv : *powerMap) {
        auto const& pod = kv.second;
        if (pod._M_start == pod._M_finish) {
            toErase.push_back(kv.first);
        }
    }

    // 4) 真正删除
    for (auto const& pos : toErase) {
        powerMap->erase(pos);
    }
}

// --------------------------------------------------
// 安装 Hook
// --------------------------------------------------
void installHook() {
    constexpr char const* sym = "?removeStaleRelationships@CircuitSceneGraph@@AEAAXXZ";
    void* target = dlsym_real(sym);
    if (!target) {
        #ifdef _DEBUG
        Logger("PowerLeakFix").error("找不到符号 {}", sym);
        #endif
        return;
    }
    HookFunction(
        target,
        reinterpret_cast<void**>(&orig_removeStale),
        reinterpret_cast<void*>(&hooked_removeStaleRelationships)
    );
    #ifdef _DEBUG
    Logger("PowerLeakFix").info("PowerAssociationMap 漏泄修复已安装");
    #endif
}

} // namespace PowerAssociationMapLeakFix
