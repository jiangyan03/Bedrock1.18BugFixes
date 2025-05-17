//plugin.cpp
#include "pch.h"
#include <EventAPI.h>
#include <LoggerAPI.h>
#include <MC/Level.hpp>
#include <MC/BlockInstance.hpp>
#include <MC/Block.hpp>
#include <MC/BlockSource.hpp>
#include <MC/Actor.hpp>
#include <MC/Player.hpp>
#include <MC/ItemStack.hpp>
#include <LLAPI.h>
// #include <MC/CircuitSceneGraph.hpp>
// #include <MC/BaseCircuitComponent.hpp>
#include "PowerAssociationMapLeakFix.h"  

//  Logger
Logger logger("BedrockArgon");

inline void CheckProtocolVersion() {
    #ifdef TARGET_BDS_PROTOCOL_VERSION
        auto currentProtocol = LL::getServerProtocolVersion();
        if (TARGET_BDS_PROTOCOL_VERSION != currentProtocol)
        {
            logger.warn("Protocol version not match, target version: {}, current version: {}.",
                TARGET_BDS_PROTOCOL_VERSION, currentProtocol);
            logger.warn("This will most likely crash the server, please use the Plugin that matches the BDS version!");
        }
    #endif // TARGET_BDS_PROTOCOL_VERSION
}


void PluginInit() {
    // 
    logger.setFile("logs/BedrockArgon.log"); 
    logger.info("try Hook");
    CheckProtocolVersion();
    try {
        if (!PowerAssociationMapLeakFix::installHook()) {
            logger.error("Failed to install hook. Plugin will not continue.");
            return;
        }
    } catch (const std::exception& e) {
        logger.fatal("Exception during installHook: {}", e.what());
        throw;
    }

    logger.info("Plugin initialized. Hook activated!");
}
