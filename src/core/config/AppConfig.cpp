#include "core/config/AppConfig.h"

#include <windows.h>
#include <objbase.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "core/config/Json.h"

namespace bm {

namespace {

std::string StringOr(const json::Value& object, const char* key, std::string fallback = {}) {
  const json::Value& field = object[key];
  return field.GetType() == json::Value::Type::String ? field.AsString() : std::move(fallback);
}

int IntOr(const json::Value& object, const char* key, int fallback) {
  const json::Value& field = object[key];
  return field.GetType() == json::Value::Type::Number ? static_cast<int>(field.AsInt()) : fallback;
}

// 布局时钟是秒级 64 位值，不能用 int 截断
int64_t ClockOr(const json::Value& object, const char* key) {
  const json::Value& field = object[key];
  return field.GetType() == json::Value::Type::Number ? field.AsInt() : 0;
}

bool BoolOr(const json::Value& object, const char* key, bool fallback) {
  const json::Value& field = object[key];
  return field.GetType() == json::Value::Type::Bool ? field.AsBool() : fallback;
}

std::string MachineName() {
  wchar_t buffer[256];
  DWORD size = static_cast<DWORD>(std::size(buffer));
  if (!GetComputerNameW(buffer, &size)) return "COMPUTER";
  return ToUtf8(std::wstring_view(buffer, size));
}

std::vector<ManualPeer> ReadPeers(const json::Value& array) {
  std::vector<ManualPeer> peers;
  if (array.GetType() != json::Value::Type::Array) return peers;
  for (const json::Value& item : array.AsArray()) {
    ManualPeer peer;
    peer.name = StringOr(item, "Name");
    peer.address = StringOr(item, "Address");
    peer.port = IntOr(item, "Port", 35100);
    peers.push_back(std::move(peer));
  }
  return peers;
}

json::Value WritePeers(const std::vector<ManualPeer>& peers) {
  json::Value array = json::Value::MakeArray();
  for (const ManualPeer& peer : peers) {
    json::Value item = json::Value::MakeObject();
    item.Set("Name", peer.name);
    item.Set("Address", peer.address);
    item.Set("Port", peer.port);
    array.Push(std::move(item));
  }
  return array;
}

}  // namespace

std::string AppConfig::FilePath() {
  if (const char* custom = std::getenv("BORDERLESS_CONFIG"); custom && *custom) return custom;
  const char* appData = std::getenv("APPDATA");
  if (!appData || !*appData) throw std::runtime_error("无法取得 APPDATA 配置目录");
  return std::string(appData) + "\\BorderlessMouse\\config.json";
}

AppConfig AppConfig::Load() {
  const std::filesystem::path path(ToWide(FilePath()));
  AppConfig config;
  if (std::ifstream file(path, std::ios::binary); file) {
    std::string text;
    char buffer[8192];
    while (file.read(buffer, sizeof(buffer))) text.append(buffer, sizeof(buffer));
    if (file.bad() || !file.eof()) throw std::runtime_error("配置文件读取失败");
    text.append(buffer, static_cast<size_t>(file.gcount()));
    // 配置包含本机身份和固定公钥，损坏时必须停止，不能悄悄重建信任关系
    config = Deserialize(text);
  } else {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
      throw std::runtime_error("无法打开现有配置文件");
    config.deviceName = MachineName();
  }
  // 同机多开调试时用环境变量区分设备名与端口
  if (const char* name = std::getenv("BORDERLESS_DEVICE_NAME"); name && *name) config.deviceName = name;
  if (const char* port = std::getenv("BORDERLESS_PORT"); port && *port) config.port = std::atoi(port);
  return config;
}

void AppConfig::Save() const {
  const std::string text = Serialize();
  const std::filesystem::path path(ToWide(FilePath()));
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
  GUID id{};
  wchar_t suffix[39]{};
  if (FAILED(CoCreateGuid(&id)) || StringFromGUID2(id, suffix, static_cast<int>(std::size(suffix))) == 0)
    throw std::runtime_error("无法生成临时配置文件名");
  const std::filesystem::path temporary(path.native() + suffix + L".tmp");

  // 同目录的独立临时文件完整关闭后再替换，失败时保留原配置。
  try {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("无法替换配置文件，错误码 " + std::to_string(GetLastError()));
  } catch (...) {
    DeleteFileW(temporary.c_str());
    throw;
  }
}

std::string AppConfig::Serialize() const {
  json::Value root = json::Value::MakeObject();
  root.Set("DeviceName", deviceName);
  root.Set("Port", port);
  root.Set("ClipboardSync", clipboardSync);
  root.Set("HideCursorWhileRemote", hideCursorWhileRemote);
  root.Set("ClipboardLimitMb", clipboardLimitMb);
  root.Set("MouseDpi", mouseDpi);

  json::Value settings = json::Value::MakeArray();
  for (const DeviceSetting& setting : deviceSettings) {
    json::Value item = json::Value::MakeObject();
    item.Set("Id", setting.id);
    item.Set("Alias", setting.alias);
    item.Set("WheelReverse", setting.wheelReverse);
    item.Set("WheelScalePercent", setting.wheelScalePercent);
    item.Set("PointerDpi", setting.pointerDpi);
    item.Set("Clipboard", setting.clipboard);
    settings.Push(std::move(item));
  }
  root.Set("DeviceSettings", std::move(settings));
  root.Set("ManualPeers", WritePeers(manualPeers));
  root.Set("TrustedPeers", WritePeers(savedAddresses));

  json::Value paired = json::Value::MakeArray();
  for (const PairedPeer& peer : pairedPeers) {
    json::Value item = json::Value::MakeObject();
    item.Set("Id", peer.id);
    item.Set("Name", peer.name);
    item.Set("PublicKey", peer.publicKey);
    item.Set("Address", peer.address);
    item.Set("Port", peer.port);
    paired.Push(std::move(item));
  }
  root.Set("PairedPeers", std::move(paired));
  if (!protectedIdentityKey.empty()) root.Set("ProtectedIdentityKey", protectedIdentityKey);

  if (hasTopology) {
    json::Value topologyJson = json::Value::MakeObject();
    topologyJson.Set("Clock", topology.clock);
    topologyJson.Set("AuthorId", topology.authorId);
    json::Value devices = json::Value::MakeArray();
    for (const TopologyDevice& device : topology.devices) {
      json::Value item = json::Value::MakeObject();
      item.Set("DeviceId", device.deviceId);
      item.Set("Name", device.name);
      item.Set("Row", device.row);
      item.Set("Column", device.column);
      item.Set("Clock", device.clock);
      item.Set("AuthorId", device.authorId);
      item.Set("State", static_cast<int>(device.state));
      devices.Push(std::move(item));
    }
    topologyJson.Set("Devices", std::move(devices));
    root.Set("Topology", std::move(topologyJson));
  }
  return root.Dump();
}

AppConfig AppConfig::Deserialize(std::string_view text) {
  json::Value root = json::Value::Parse(text);
  if (root.GetType() != json::Value::Type::Object) throw std::runtime_error("配置内容不是对象，无法读取设备身份");
  AppConfig config;
  config.deviceName = StringOr(root, "DeviceName");
  config.port = IntOr(root, "Port", 35100);
  config.clipboardSync = BoolOr(root, "ClipboardSync", false);
  config.hideCursorWhileRemote = BoolOr(root, "HideCursorWhileRemote", true);
  config.clipboardLimitMb = IntOr(root, "ClipboardLimitMb", 64);
  config.mouseDpi = IntOr(root, "MouseDpi", 1600);
  config.protectedIdentityKey = StringOr(root, "ProtectedIdentityKey");
  config.manualPeers = ReadPeers(root["ManualPeers"]);
  config.savedAddresses = ReadPeers(root["TrustedPeers"]);

  if (const json::Value& settings = root["DeviceSettings"]; settings.GetType() == json::Value::Type::Array) {
    for (const json::Value& item : settings.AsArray()) {
      DeviceSetting setting;
      setting.id = StringOr(item, "Id");
      setting.alias = StringOr(item, "Alias");
      setting.wheelReverse = BoolOr(item, "WheelReverse", false);
      setting.wheelScalePercent = IntOr(item, "WheelScalePercent", 100);
      setting.pointerDpi = IntOr(item, "PointerDpi", 0);
      setting.clipboard = BoolOr(item, "Clipboard", true);
      config.deviceSettings.push_back(std::move(setting));
    }
  }
  if (const json::Value& paired = root["PairedPeers"]; paired.GetType() == json::Value::Type::Array) {
    for (const json::Value& item : paired.AsArray()) {
      PairedPeer peer;
      peer.id = StringOr(item, "Id");
      peer.name = StringOr(item, "Name");
      peer.publicKey = StringOr(item, "PublicKey");
      peer.address = StringOr(item, "Address");
      peer.port = IntOr(item, "Port", 35100);
      config.pairedPeers.push_back(std::move(peer));
    }
  }
  if (const json::Value& topology = root["Topology"]; topology.GetType() == json::Value::Type::Object) {
    config.topology.clock = ClockOr(topology, "Clock");
    config.topology.authorId = StringOr(topology, "AuthorId");
    const json::Value& devices = topology["Devices"];
    for (const json::Value& item : devices.GetType() == json::Value::Type::Array ? devices.AsArray() : std::vector<json::Value>{}) {
      TopologyDevice device;
      device.deviceId = StringOr(item, "DeviceId");
      device.name = StringOr(item, "Name");
      device.row = IntOr(item, "Row", 0);
      device.column = IntOr(item, "Column", 0);
      device.clock = ClockOr(item, "Clock");
      device.authorId = StringOr(item, "AuthorId");
      device.state = static_cast<TopologyDeviceState>(IntOr(item, "State", 0));
      config.topology.devices.push_back(std::move(device));
    }
    config.hasTopology = TopologyCodec::IsValid(config.topology);
  }
  return config;
}

}  // namespace bm
