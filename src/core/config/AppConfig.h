#pragma once

#include "core/platform/Common.h"
#include "core/layout/Topology.h"

#include <string>
#include <vector>

namespace bm {

// 一台设备的本地设置：别名、滚轮与指针换算
struct DeviceSetting {
  std::string id;
  std::string alias;
  bool wheelReverse = false;
  int wheelScalePercent = 100;
  int pointerDpi = 0;  // 0 表示与本机一致
  bool clipboard = true;
};

// 手动添加过的对端；旧版本的地址记录只作为待配对候选
struct ManualPeer {
  std::string name;
  std::string address;
  int port = 35100;
};

// 公钥固定后的设备，名称和地址变化不改变认证身份
struct PairedPeer {
  std::string id;
  std::string name;
  std::string publicKey;
  std::string address;
  int port = 35100;
};

// 落盘配置：%APPDATA%\BorderlessMouse\config.json，与 .NET 版可互换
struct AppConfig {
  std::string deviceName;
  int port = 35100;
  bool clipboardSync = false;
  bool hideCursorWhileRemote = true;
  int clipboardLimitMb = 64;  // 文本与图片单条上限（1–64 MB）
  int mouseDpi = 1600;
  std::vector<DeviceSetting> deviceSettings;
  std::vector<ManualPeer> manualPeers;
  std::vector<ManualPeer> savedAddresses;  // JSON 中名为 TrustedPeers
  std::string protectedIdentityKey;        // DPAPI 加密的 PKCS8 私钥
  std::vector<PairedPeer> pairedPeers;
  TopologySnapshot topology;
  bool hasTopology = false;

  static std::string FilePath();
  static AppConfig Load();
  void Save() const;

  std::string Serialize() const;
  static AppConfig Deserialize(std::string_view text);
};

}  // namespace bm
