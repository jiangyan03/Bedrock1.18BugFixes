// PowerAssociationMapLeakFix.cpp
#include "PowerAssociationMapLeakFix.h"
#include <HookAPI.h>
#include <LoggerAPI.h>
#include <MC/CircuitSceneGraph.hpp>
#include <cstdint>

extern Logger logger;

// 原始函数指针
static void (__fastcall *original_removeStaleRelationships)(CircuitSceneGraph*);

// 为了验证偏移没写错，保留探测输出
static constexpr size_t OFFSETS[] = {
    0x120, // mPendingRemovals 链表哨兵
    0xA0,  // mPendingAdds._M_end_of_bucket_list
    0xB0,  // mPendingAdds._M_buckets
    0xC8,  // mPendingAdds._M_mask
    0x08,  // mComponents._M_end_of_bucket_list
    0x18,  // mComponents._M_buckets
    0x30   // mComponents._M_mask
};

static void __fastcall hooked_removeStaleRelationships(CircuitSceneGraph* self) {
    auto ptr = reinterpret_cast<uintptr_t>(self);
    // logger.info(">> [HookProbe] removeStaleRelationships START @0x{:X}", ptr);

    // 打印我们关心的几个内存偏移
    for (size_t off : OFFSETS) {
        void* addr = (char*)self + off;
        uintptr_t val = 0;
        bool ok = true;
        __try {
            val = *reinterpret_cast<uintptr_t*>(addr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        if (ok) {
            // logger.info("   off 0x{:03X}: addr=0x{:X}, val=0x{:X}",
            //             off, (uintptr_t)addr, val);
        } else {
            // logger.warn("   off 0x{:03X}: addr=0x{:X} ACCESS_VIOLATION",
            //             off, (uintptr_t)addr);
        }
    }

    // 调用原版逻辑，保留正常的“削枝”行为
    original_removeStaleRelationships(self);

    // —— 激进修复 —— 
    // 直接把 pendingRemovals 链表重置为空表
    auto headPtr = reinterpret_cast<uintptr_t*>((char*)self + 0x120);
    uintptr_t head = reinterpret_cast<uintptr_t>(headPtr);
    __try {
        // headPtr 存储的是哨兵节点的地址
        // 让它指向自己，表明“空链表”
        *headPtr = head;
        // 同时把哨兵节点里的 next 指针也指回自己
        // （在 removeStaleRelationships 里它只会读 [head] 作为首节点）
        *reinterpret_cast<uintptr_t*>(head) = head;
        logger.info("[LeakFix] mPendingRemovals 已重置为空链表");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logger.error("[LeakFix] 无法重置 mPendingRemovals，内存访问异常");
    }

    // logger.info("<< [HookProbe] removeStaleRelationships END   @0x{:X}", ptr);
}

namespace PowerAssociationMapLeakFix {
    void installHook() {
        constexpr const char* sym = "?removeStaleRelationships@CircuitSceneGraph@@AEAAXXZ";
        void* target = dlsym_real(sym);
        if (!target) {
            logger.error("LeakFix: 无法解析符号 {}", sym);
            return;
        }
        void* orig = nullptr;
        int   r    = HookFunction(target, &orig, (void*)&hooked_removeStaleRelationships);
        if (r != 0) {
            logger.error("LeakFix: HookFunction 失败, code={}", r);
            return;
        }
        original_removeStaleRelationships =
            reinterpret_cast<decltype(original_removeStaleRelationships)>(orig);
        logger.info("LeakFix: 已在 0x{:X} 安装 removeStaleRelationships 钩子", (uintptr_t)target);
    }
}
