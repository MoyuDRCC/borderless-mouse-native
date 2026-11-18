#pragma once

namespace bm {

// 开机启动写在当前用户的 Run 键，不依赖计划任务或服务
bool IsAutoStartEnabled();
void SetAutoStart(bool enabled);

}  // namespace bm
