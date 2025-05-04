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

#include <MC/CircuitSceneGraph.hpp>
#include <MC/BaseCircuitComponent.hpp>
#include <LLAPI.h>
#include "PowerAssociationMapLeakFix.h"  

//  Logger
Logger logger("PowerAssociationMapLeakFix");

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
    logger.setFile("logs/PowerAssociationMapLeakFix.log"); 
    CheckProtocolVersion();
    
    // Hook
    PowerAssociationMapLeakFix::installHook();
    logger.info("Plugin initialized. Hook activated!");
}
