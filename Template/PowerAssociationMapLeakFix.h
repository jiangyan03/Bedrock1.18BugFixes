#pragma once
#include "pch.h"
#include <LoggerAPI.h>

namespace PowerAssociationMapLeakFix {
  extern Logger logger; // 外部声明
  bool installHook();    // 返回 bool在plugin.cpp 中调用
  // bool setHash3FromSymbol(const char* symbolName); // 可选：使用真实 hash 函数

}
