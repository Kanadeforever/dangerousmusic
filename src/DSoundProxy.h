// 系统 DSOUND 代理声明。
#pragma once
#include "Common.h"

namespace localmusic::dsound_proxy {

// 确保系统真实 dsound.dll 已加载且所需导出已解析。
// 代理导出在转发前调用该方法。
bool EnsureLoaded();
// 释放或保留系统 DirectSound 模块的退出策略入口。
// 当前实现为避免晚期调用而保留模块到进程结束。
void Unload();

}  // namespace localmusic::dsound_proxy
