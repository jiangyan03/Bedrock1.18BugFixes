// PowerAssociationMapLeakFix.cpp
#include "PowerAssociationMapLeakFix.h"
#include <HookAPI.h>
#include <LoggerAPI.h>
#include <MC/CircuitSceneGraph.hpp>
#include <MC/BaseCircuitComponent.hpp>
#include <MC/BlockPos.hpp>
#include <array>
#include <cstdint>

extern Logger logger;

// 原 removeStaleRelationships 函数指针
static void (__fastcall *original_removeStaleRelationships)(CircuitSceneGraph*);

// 需要探测的偏移列表
static constexpr std::array<size_t,7> OFFSETS = {{
    0x120, // mPendingRemovals 链表头
    0x0A0, // mPendingAdds._M_end_of_bucket_list
    0x0B0, // mPendingAdds._M_buckets
    0x0C8, // mPendingAdds._M_mask
    0x008, // mComponents._M_end_of_bucket_list
    0x018, // mComponents._M_buckets
    0x030  // mComponents._M_mask
}};

// 打印偏移探测日志
static void probeOffsets(CircuitSceneGraph* thiz, const char* tag) {
    logger.info("{} [HookProbe] removeStaleRelationships {} ({:p})",
                (tag[0]=='<'?"<<":"") , tag, (void*)thiz);
    for (auto off : OFFSETS) {
        void* addr = reinterpret_cast<char*>(thiz) + off;
        uintptr_t val = 0;
        bool ok = true;
        __try {
            val = *reinterpret_cast<uintptr_t*>(addr);
        } __except (GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION
                     ? EXCEPTION_EXECUTE_HANDLER
                     : EXCEPTION_CONTINUE_SEARCH) {
            ok = false;
        }
        // if (ok) {
        //     logger.info("    off 0x{:03X}: addr={:p}, val={:p}", off, addr, (void*)val);
        // } else {
        //     logger.warn("    off 0x{:03X}: addr={:p}, ACCESS VIOLATION", off, addr);
        // }
    }
}

// 手工遍历 this+0x120 的双向链表，清理空的节点(begin==end)
static void cleanPendingRemovals(CircuitSceneGraph* thiz) {
    // 1) 取出头指针
    void* head = *reinterpret_cast<void**>(reinterpret_cast<char*>(thiz) + 0x120);
    if (!head) {
        logger.warn("[LeakFix] head@0x120 == nullptr, skip");
        return;
    }
    // 2) head->prev 存放在 head+0x00
    void* node = *reinterpret_cast<void**>(head);
    if (node == head) {
        // 链表空
        logger.info("[LeakFix] mPendingRemovals is empty");
        return;
    }
    int idx = 0;
    // 3) 逆序遍历：while(node != head) { ... node = node->prev; }
    while (node != head) {
        void* prev = *reinterpret_cast<void**>(node);          // node->prev
        void* begin = *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x20);
        void* end   = *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x28);
        logger.info("[LeakFix] Node[{}] @{:p}  begin={:p}, end={:p}", idx, node, begin, end);
        if (begin == end) {
            // 已经是空的，无需操作
        } else {
            // 如果要更严格判断「真正都死了」可以遍历 begin..end 调用 removeSource 等
            // 这里我们直接把它清空，避免野内存
            logger.info("[LeakFix]   clearing node[{}]", idx);
            *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x20) = nullptr;
            *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x28) = nullptr;
        }
        idx++;
        node = prev;
    }
    logger.info("[LeakFix] cleaned {} nodes", idx);
}

// Hook 入口：探测→原调用→清理→探测
static void __fastcall hooked_removeStaleRelationships(CircuitSceneGraph* thiz) {
    probeOffsets(thiz, "START");
    original_removeStaleRelationships(thiz);
    cleanPendingRemovals(thiz);
    probeOffsets(thiz, "END");
}

namespace PowerAssociationMapLeakFix {
    void installHook() {
        constexpr char const* sym = "?removeStaleRelationships@CircuitSceneGraph@@AEAAXXZ";
        void* target = dlsym_real(sym);
        if (!target) {
            logger.error("Failed to resolve symbol {}", sym);
            return;
        }
        void* orig = nullptr;
        int r = HookFunction(target, &orig, (void*)&hooked_removeStaleRelationships);
        if (r != 0) {
            logger.error("HookFunction failed: {}", r);
            return;
        }
        original_removeStaleRelationships =
            reinterpret_cast<decltype(original_removeStaleRelationships)>(orig);
        logger.info("Probe-hook installed on removeStaleRelationships @{:p}", target);
    }
}