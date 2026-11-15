#include "crypto/device_identity.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "crypto/hash.h"

#if RC_ENABLE_TLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#endif

namespace rc {
namespace {

// pub_key 只允许 base64 的小字符集，长度上限防止对端用超长字段拖慢解析。
constexpr std::size_t kMaxKeyText = 2048;
constexpr std::size_t kMaxSignatureText = 1024;

std::string identity_path(const std::string& path) {
  return path.empty() ? DeviceIdentity::default_path() : path;
}

bool ensure_parent_dir(const std::string& path, std::string* error) {
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (parent.empty()) return true;
  std::error_code ec;
  std::filesystem::create_directories(parent, ec);
  if (ec) {
    if (error) *error = "无法创建身份目录 " + parent.string() + ": " + ec.message();
    return false;
  }
  return true;
}

}  // namespace

struct DeviceIdentity::Impl {
#if RC_ENABLE_TLS
  mbedtls_pk_context pk{};
  mbedtls_entropy_context entropy{};
  mbedtls_ctr_drbg_context drbg{};
  bool rng_ready = false;

  Impl() {
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    static const unsigned char kSeed[] = "rc-device-identity-v1";
    rng_ready = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                      kSeed, sizeof(kSeed) - 1) == 0;
  }
  ~Impl() {
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
  }
#endif
};

DeviceIdentity::DeviceIdentity() : impl_(std::make_unique<Impl>()) {}
DeviceIdentity::~DeviceIdentity() = default;
DeviceIdentity::DeviceIdentity(DeviceIdentity&&) noexcept = default;
DeviceIdentity& DeviceIdentity::operator=(DeviceIdentity&&) noexcept = default;

bool DeviceIdentity::available() {
#if RC_ENABLE_TLS
  return true;
#else
  return false;
#endif
}

std::string DeviceIdentity::default_path() {
#ifdef _WIN32
  const char* base = std::getenv("APPDATA");
  const std::filesystem::path root =
      (base && base[0] != '\0') ? std::filesystem::path(base)
                                : std::filesystem::path(".");
#else
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  std::filesystem::path root;
  if (xdg && xdg[0] != '\0') {
    root = xdg;
  } else {
    const char* home = std::getenv("HOME");
    root = (home && home[0] != '\0') ? std::filesystem::path(home) / ".config"
                                     : std::filesystem::path(".");
  }
#endif
  return (root / "rc-desktop" / "identity.pem").string();
}

bool DeviceIdentity::export_public_key(std::string* error) {
#if RC_ENABLE_TLS
  unsigned char der[512];
  const int len = mbedtls_pk_write_pubkey_der(&impl_->pk, der, sizeof(der));
  if (len <= 0) {
    if (error) *error = "导出设备公钥失败";
    return false;
  }
  const unsigned char* const begin = der + sizeof(der) - static_cast<std::size_t>(len);
  const unsigned char* const end = der + sizeof(der);
  public_key_b64_ = base64_encode(std::vector<uint8_t>(begin, end));
  return true;
#else
  if (error) *error = "该构建未启用 mbed TLS，无法使用设备身份";
  return false;
#endif
}

bool DeviceIdentity::load(const std::string& path, std::string* error) {
#if RC_ENABLE_TLS
  const std::string file = identity_path(path);
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) return false;

  if (impl_->rng_ready == false) {
    if (error) *error = "随机数初始化失败，无法读取设备身份";
    return false;
  }
  mbedtls_pk_free(&impl_->pk);
  mbedtls_pk_init(&impl_->pk);
  if (mbedtls_pk_parse_keyfile(&impl_->pk, file.c_str(), "",
                               mbedtls_ctr_drbg_random, &impl_->drbg) != 0) {
    if (error) *error = "设备身份文件无法解析: " + file;
    return false;
  }
  return export_public_key(error);
#else
  (void)path;
  if (error) *error = "该构建未启用 mbed TLS，无法使用设备身份";
  return false;
#endif
}

bool DeviceIdentity::load_or_create(const std::string& path, std::string* error) {
#if RC_ENABLE_TLS
  const std::string file = identity_path(path);
  std::error_code ec;
  const bool exists = std::filesystem::exists(file, ec);

  if (load(file, error)) return true;
  // 文件已存在却读不出来时不能静默重建：换密钥会被中继判定为顶号。
  if (exists) return false;

  if (!ensure_parent_dir(file, error)) return false;
  if (!impl_->rng_ready) {
    if (error) *error = "随机数初始化失败，无法生成设备身份";
    return false;
  }

  mbedtls_pk_free(&impl_->pk);
  mbedtls_pk_init(&impl_->pk);
  if (mbedtls_pk_setup(&impl_->pk,
                       mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
      mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(impl_->pk),
                          mbedtls_ctr_drbg_random, &impl_->drbg) != 0) {
    if (error) *error = "生成设备身份密钥失败";
    return false;
  }

  unsigned char pem[16000];
  if (mbedtls_pk_write_key_pem(&impl_->pk, pem, sizeof(pem)) != 0) {
    if (error) *error = "写出设备身份失败";
    return false;
  }
  const std::size_t pem_len = std::strlen(reinterpret_cast<const char*>(pem));
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "无法写入设备身份文件: " + file;
      return false;
    }
    out.write(reinterpret_cast<const char*>(pem), static_cast<std::streamsize>(pem_len));
    if (!out.good()) {
      if (error) *error = "设备身份文件写入不完整: " + file;
      return false;
    }
  }
  // 私钥只允许当前用户读写（Windows 上该调用无效果，交给 ACL）。
  std::filesystem::permissions(file,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, ec);
  return export_public_key(error);
#else
  (void)path;
  if (error) *error = "该构建未启用 mbed TLS，无法使用设备身份";
  return false;
#endif
}

bool DeviceIdentity::sign(const std::string& data, std::string& signature_b64,
                          std::string* error) const {
#if RC_ENABLE_TLS
  if (!valid()) {
    if (error) *error = "设备身份尚未加载";
    return false;
  }
  const std::vector<uint8_t> digest = sha256(data);
  unsigned char signature[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
  std::size_t signature_len = 0;
  const int rc = mbedtls_pk_sign(
      &impl_->pk, MBEDTLS_MD_SHA256, digest.data(), digest.size(), signature,
      sizeof(signature), &signature_len, mbedtls_ctr_drbg_random, &impl_->drbg);
  if (rc != 0) {
    if (error) *error = "设备身份签名失败";
    return false;
  }
  signature_b64 = base64_encode(
      std::vector<uint8_t>(signature, signature + signature_len));
  return true;
#else
  (void)data;
  (void)signature_b64;
  if (error) *error = "该构建未启用 mbed TLS，无法使用设备身份";
  return false;
#endif
}

bool DeviceIdentity::verify(const std::string& public_key_b64,
                            const std::string& data,
                            const std::string& signature_b64,
                            std::string* error) {
#if RC_ENABLE_TLS
  if (public_key_b64.empty() || public_key_b64.size() > kMaxKeyText ||
      signature_b64.empty() || signature_b64.size() > kMaxSignatureText) {
    if (error) *error = "设备公钥或签名格式无效";
    return false;
  }

  std::vector<uint8_t> key;
  std::vector<uint8_t> signature;
  try {
    key = base64_decode(public_key_b64);
    signature = base64_decode(signature_b64);
  } catch (const std::exception&) {
    if (error) *error = "设备公钥或签名不是合法 base64";
    return false;
  }
  if (key.empty() || signature.empty()) {
    if (error) *error = "设备公钥或签名为空";
    return false;
  }

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int rc = mbedtls_pk_parse_public_key(&pk, key.data(), key.size());
  if (rc == 0) {
    const std::vector<uint8_t> digest = sha256(data);
    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest.data(), digest.size(),
                           signature.data(), signature.size());
  }
  mbedtls_pk_free(&pk);
  if (rc != 0) {
    if (error) *error = "设备身份签名不匹配";
    return false;
  }
  return true;
#else
  (void)public_key_b64;
  (void)data;
  (void)signature_b64;
  if (error) *error = "该构建未启用 mbed TLS，无法校验设备身份";
  return false;
#endif
}

}  // namespace rc
