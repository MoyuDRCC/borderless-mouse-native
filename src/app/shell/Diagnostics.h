#pragma once

#include <string>

#include "app/UiModel.h"

namespace bm {

struct AppConfig;

// 导出诊断文本：版本、系统、端口、配对数、会话与开关，随后是最近 200 行日志
std::string BuildDiagnostics(const UiModel& model, const AppConfig& config);
// 弹出保存对话框并写盘；返回 false 表示用户取消
bool ExportDiagnostics(const UiModel& model, const AppConfig& config);

}  // namespace bm
