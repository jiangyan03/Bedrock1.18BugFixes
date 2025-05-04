// PowerAssociationMapLeakFix.cpp
#include "PowerAssociationMapLeakFix.h"
#include <HookAPI.h>
#include <LoggerAPI.h>
#include <MC/CircuitSceneGraph.hpp>

extern Logger logger;

// 原始函数指针
static void (__fastcall *original_removeStaleRelationships)(CircuitSceneGraph*);

// 需要探测的偏移列表
static constexpr size_t OFFSETS[] = {
    0x120, // mPendingRemovals 链表头
    0xA0,  // mPendingAdds._M_end_of_bucket_list
    0xB0,  // mPendingAdds._M_buckets
    0xC8,  // mPendingAdds._M_mask
    0x08,  // mComponents._M_end_of_bucket_list
    0x18,  // mComponents._M_buckets
    0x30   // mComponents._M_mask
};

static void __fastcall hooked_removeStaleRelationships(CircuitSceneGraph* thisPtr) {
    if (!thisPtr) {
        logger.error("[HookProbe] thisPtr is null, skipping");
    } else {
        logger.info(">> [HookProbe] removeStaleRelationships START (this={:p})", (void*)thisPtr);
        for (size_t off : OFFSETS) {
            void* addr = reinterpret_cast<char*>(thisPtr) + off;
            uintptr_t val = 0;
            bool ok = true;
            __try {
                val = *reinterpret_cast<uintptr_t*>(addr);
            } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                         ? EXCEPTION_EXECUTE_HANDLER
                         : EXCEPTION_CONTINUE_SEARCH) {
                ok = false;
            }
            if (ok) {
                logger.info("    offset 0x{:03X}: addr={:p}, value={:p}", off, addr, (void*)val);
            } else {
                logger.warn("    offset 0x{:03X}: addr={:p}, ACCESS VIOLATION", off, addr);
            }
        }
    }

    // 调用原始函数保持逻辑不变
    original_removeStaleRelationships(thisPtr);

    if (thisPtr) {
        logger.info("<< [HookProbe] removeStaleRelationships END (this={:p})", (void*)thisPtr);
    }
}

namespace PowerAssociationMapLeakFix {

void installHook() {
    constexpr const char* sym = "?removeStaleRelationships@CircuitSceneGraph@@AEAAXXZ";
    void* target = dlsym_real(sym);
    if (!target) {
        logger.error("Failed to resolve symbol {}", sym);
        return;
    }
    void* origPtr = nullptr;
    int ret = HookFunction(
        target,
        &origPtr,
        reinterpret_cast<void*>(&hooked_removeStaleRelationships)
    );
    if (ret != 0) {
        logger.error("HookFunction failed with code {}", ret);
        return;
    }
    original_removeStaleRelationships =
        reinterpret_cast<void (__fastcall *)(CircuitSceneGraph*)>(origPtr);
    logger.info("Probe-hook installed: removeStaleRelationships@{:p}", target);
}

}  // namespace PowerAssociationMapLeakFix
