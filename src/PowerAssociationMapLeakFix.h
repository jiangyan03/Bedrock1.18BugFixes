#pragma once
#include "pch.h"
#include <LoggerAPI.h>
/*
  BDS 1.18.2 下 CircuitSceneGraph 的内存布局（this + offset）：
    0x00  0  ── mAllComponents            : std::unordered_map<BlockPos, std::unique_ptr<BaseCircuitComponent>>  ComponentMap
    0xB0  88 ── mActiveComponentsPerChunk : std::unordered_map<BlockPos, CircuitComponentList>                   ComponentsPerChunkMap
    0x98  152── mPowerAssociationMap      : std::unordered_map<BlockPos, CircuitComponentList>                   ComponentsPerPosMap
    0xD8  216── mPendingAdds              : std::unordered_map<BlockPos, CircuitSceneGraph::PendingEntry>
    0x120 288── mPendingUpdates_relList   : std::unordered_map<BlockPos, CircuitSceneGraph::PendingEntry>
    0x118 280── mPendingUpdates
    0x158 344── mComponentsToReEvaluate   : std::vector<BlockPos>
    0x198 408── mPendingRemoves           : std::vector<CircuitSceneGraph::PendingEntry>
*/
// PowerAssociationMapLeakFix.h

extern Logger logger;
namespace PowerAssociationMapLeakFix {
  bool installHook();    // 返回 bool在plugin.cpp 中调用
}
