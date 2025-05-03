// PowerAssociationMapLeakFix.cpp
#include "PowerAssociationMapLeakFix.h"
#include "HookAPI.h"
#include "LoggerAPI.h"
#include <MC/CircuitSceneGraph.hpp>
#include <MC/BlockPos.hpp>

// use TInstanceHook 
TInstanceHook(void, "?removeComponent@CircuitSceneGraph@@AEAAXAEBVBlockPos@@@Z", 
    CircuitSceneGraph, const BlockPos& pos)
{
    // 
    Logger("PowerAssociationMapLeakFix").info(
        "CircuitSceneGraph::removeComponent called at ({}, {}, {})", 
        pos.x, pos.y, pos.z
    );
    
    // 
    original(this, pos);
}

void PowerAssociationMapLeakFix::installHook() {
    // Hook  TInstanceHook 
}