#pragma once

#include <memory>
#include <string>

namespace rc {

// 设备身份：EC P-256 密钥对。被控端首次启动生成并保存在本地，
// 注册到中继时用私钥对中继下发的 nonce 签名；中继按 device_id 固定公钥，
// 从而阻止他人用相同 device_id 顶号（只有同一密钥的重连才被接受）。
class DeviceIdentity {
 public:
  DeviceIdentity();
  ~DeviceIdentity();
  DeviceIdentity(DeviceIdentity&&) noexcept;
  DeviceIdentity& operator=(DeviceIdentity&&) noexcept;
  DeviceIdentity(const DeviceIdentity&) = delete;
  DeviceIdentity& operator=(const DeviceIdentity&) = delete;

  // 当前构建是否带 mbed TLS（未启用时设备身份不可用，中继退回旧行为）。
  static bool available();

  // 默认身份文件位置：Windows %APPDATA%\rc-desktop\identity.pem，
  // Linux $XDG_CONFIG_HOME/rc-desktop/identity.pem（缺省 ~/.config）。
  static std::string default_path();

  // 先尝试读取 path，不存在时生成新密钥并写入；path 为空用 default_path()。
  bool load_or_create(const std::string& path, std::string* error);
  bool load(const std::string& path, std::string* error);

  bool valid() const { return !public_key_b64_.empty(); }
  // DER SubjectPublicKeyInfo 的 base64，写入 Register.pub_key。
  const std::string& public_key() const { return public_key_b64_; }

  // ECDSA-SHA256 签名，输出 DER 的 base64。
  bool sign(const std::string& data, std::string& signature_b64,
            std::string* error) const;

  static bool verify(const std::string& public_key_b64, const std::string& data,
                     const std::string& signature_b64, std::string* error);

 private:
  struct Impl;
  bool export_public_key(std::string* error);

  std::unique_ptr<Impl> impl_;
  std::string public_key_b64_;
};

}  // namespace rc
