// PowerAssociationMapLeakFix.h
#pragma once
#include "pch.h"
#include <MC/CircuitSceneGraph.hpp>
// // 根据反汇编定义内部结构
/*
  BDS 1.18.2 下 CircuitSceneGraph 的内存布局（this + offset）：
    0x40 ── mAllComponents            : std::unordered_map<BlockPos, std::unique_ptr<BaseCircuitComponent>>
    0x58 ── mPowerAssociationMap      : std::unordered_map<BlockPos, CircuitComponentList>
    0x98 ── mActiveComponentsPerChunk : std::unordered_map<BlockPos, CircuitComponentList>
    0xD8 ── mPendingAdds              : std::unordered_map<BlockPos, CircuitSceneGraph::PendingEntry>
    0x118── mPendingUpdates           : std::unordered_map<BlockPos, CircuitSceneGraph::PendingEntry>
    0x170── mComponentsToReEvaluate   : std::vector<BlockPos>
    0x198── mPendingRemoves           : std::vector<CircuitSceneGraph::PendingEntry>
*/

namespace PowerAssociationMapLeakFix {
    void installHook(); // 在 plugin.cpp 中调用
}
