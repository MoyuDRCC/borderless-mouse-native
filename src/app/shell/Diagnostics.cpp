#include "app/shell/Diagnostics.h"

#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>

#include "core/config/AppConfig.h"
#include "core/platform/Common.h"

namespace bm {

namespace {

std::string LocalTimeText(uint64_t unixMs, const char* pattern) {
  std::time_t seconds = static_cast<std::time_t>(unixMs / 1000);
  std::tm local{};
  localtime_s(&local, &seconds);
  char buffer[64];
  std::strftime(buffer, sizeof(buffer), pattern, &local);
  return buffer;
}

std::string LowerAscii(std::string text) {
  for (char& c : text)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return text;
}

bool Specific(const std::string& name) {
  static const char* kGeneric[] = {"windows", "mac", "macos", "pc", "linux"};
  if (name.size() < 3) return false;
  std::string lower = LowerAscii(name);
  for (const char* generic : kGeneric)
    if (lower == generic) return false;
  return true;
}

void ReplaceAllInsensitive(std::string& text, const std::string& from, const std::string& to) {
  if (from.empty()) return;
  std::string lowerFrom = LowerAscii(from);
  size_t position = 0;
  while ((position = LowerAscii(text).find(lowerFrom, position)) != std::string::npos) {
    text.replace(position, from.size(), to);
    position += to.size();
  }
}

// 导出脱敏：本机名换成"本机"，其余依次换成"设备 A""设备 B"，用户目录换成 ~
class Redactor {
 public:
  Redactor(const std::string& localName, const std::vector<std::string>& peerNames) {
    wchar_t home[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) > 1) home_ = ToUtf8(home);
    if (Specific(localName)) names_[localName] = "本机";
    int index = 0;
    for (const std::string& name : peerNames) {
      if (!Specific(name) || names_.count(name)) continue;
      names_[name] = "设备 " + std::string(1, static_cast<char>('A' + index++));
    }
    // 长名字先替换，避免短名把长名的一部分吃掉
    std::vector<std::pair<std::string, std::string>> ordered(names_.begin(), names_.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& left, const auto& right) { return left.first.size() > right.first.size(); });
    ordered_ = std::move(ordered);
  }

  std::string Apply(std::string text) const {
    if (!home_.empty()) ReplaceAllInsensitive(text, home_, "~");
    for (const auto& [name, code] : ordered_) ReplaceAllInsensitive(text, name, code);
    return text;
  }

 private:
  std::string home_;
  std::map<std::string, std::string> names_;
  std::vector<std::pair<std::string, std::string>> ordered_;
};

}  // namespace

std::string BuildDiagnostics(const UiModel& model, const AppConfig& config) {
  int sessions = 0;
  std::vector<std::string> peerNames;
  for (const DeviceRow& device : model.devices) {
    if (device.connected) sessions++;
    if (!device.name.empty()) peerNames.push_back(device.name);
  }
  // GetVersionEx 已废弃且受清单影响，直接从 ntdll 取真实版本
  OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (auto query = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) query(&info);
  }

  std::string text;
  text += "无界键鼠 诊断\n";
  text += "时间：" + LocalTimeText(UnixMs(), "%Y-%m-%d %H:%M:%S") + "\n";
  text += "版本：" + model.version + "\n";
  text += "协议：" + std::to_string(model.protocolVersion) + "\n";
  text += "系统：Windows " + std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) +
          " build " + std::to_string(info.dwBuildNumber) + "\n";
  text += "本机：" + config.deviceName + "\n";
  text += "端口：" + std::to_string(config.port) + "\n";
  text += "已配对设备：" + std::to_string(config.pairedPeers.size()) + "\n";
  text += "活动会话：" + std::to_string(sessions) + "\n";
  text += "共享：" + std::string(model.settings.sharingEnabled ? "开" : "关") + "\n";
  text += "剪贴板同步：" + std::string(model.settings.clipboardEnabled ? "开" : "关") + "\n\n";

  size_t shown = std::min<size_t>(model.logs.size(), 200);
  text += "—— 运行日志：最后 " + std::to_string(shown) + " 行 ——\n";
  Redactor redactor(config.deviceName, peerNames);
  for (size_t i = model.logs.size() - shown; i < model.logs.size(); i++) {
    const LogEntry& entry = model.logs[i];
    const char* level = entry.level == LogLevel::Error ? "错误" : entry.level == LogLevel::Warning ? "警告" : "信息";
    text += LocalTimeText(entry.timeMs, "%Y-%m-%d %H:%M:%S") + "  " + level + "  " + redactor.Apply(entry.text) + "\n";
  }
  return text;
}

bool ExportDiagnostics(const UiModel& model, const AppConfig& config) {
  std::wstring defaultName = ToWide("无界键鼠诊断-" + LocalTimeText(UnixMs(), "%Y%m%d-%H%M") + ".txt");
  wchar_t path[MAX_PATH] = L"";
  wmemcpy(path, defaultName.c_str(), std::min(defaultName.size(), static_cast<size_t>(MAX_PATH - 1)));

  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.lpstrFilter = L"文本文件\0*.txt\0\0";
  dialog.lpstrFile = path;
  dialog.nMaxFile = MAX_PATH;
  dialog.lpstrTitle = L"导出诊断";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  if (!GetSaveFileNameW(&dialog)) return false;

  std::ofstream file(std::wstring(path), std::ios::binary | std::ios::trunc);
  std::string text = BuildDiagnostics(model, config);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return true;
}

}  // namespace bm
