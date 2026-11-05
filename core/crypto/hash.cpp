#include "crypto/hash.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <random>
#include <stdexcept>

namespace rc {
namespace {

inline uint32_t rotr(uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32 - bits));
}

const uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void compress(uint32_t state[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t t1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

std::string_view as_view(const std::vector<uint8_t>& data) {
  return std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
}

}  // namespace

std::vector<uint8_t> sha256(std::string_view data) {
  std::vector<uint8_t> message(data.begin(), data.end());
  const std::size_t original_size = message.size();
  message.push_back(0x80);
  while (message.size() % 64 != 56) message.push_back(0x00);
  const uint64_t bit_length = static_cast<uint64_t>(original_size) * 8;
  for (int i = 7; i >= 0; --i) {
    message.push_back(static_cast<uint8_t>((bit_length >> (i * 8)) & 0xFF));
  }

  uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  for (std::size_t offset = 0; offset < message.size(); offset += 64) {
    compress(state, message.data() + offset);
  }

  std::vector<uint8_t> digest(32);
  for (int i = 0; i < 8; ++i) {
    digest[i * 4 + 0] = static_cast<uint8_t>((state[i] >> 24) & 0xFF);
    digest[i * 4 + 1] = static_cast<uint8_t>((state[i] >> 16) & 0xFF);
    digest[i * 4 + 2] = static_cast<uint8_t>((state[i] >> 8) & 0xFF);
    digest[i * 4 + 3] = static_cast<uint8_t>(state[i] & 0xFF);
  }
  return digest;
}

std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key_in,
                                 const std::vector<uint8_t>& data) {
  std::vector<uint8_t> key = key_in;
  if (key.size() > 64) key = sha256(as_view(key));
  key.resize(64, 0x00);

  std::vector<uint8_t> inner_pad(64), outer_pad(64);
  for (std::size_t i = 0; i < 64; ++i) {
    inner_pad[i] = static_cast<uint8_t>(key[i] ^ 0x36);
    outer_pad[i] = static_cast<uint8_t>(key[i] ^ 0x5c);
  }

  std::vector<uint8_t> buffer(inner_pad);
  buffer.insert(buffer.end(), data.begin(), data.end());
  const std::vector<uint8_t> inner_hash = sha256(as_view(buffer));

  buffer.assign(outer_pad.begin(), outer_pad.end());
  buffer.insert(buffer.end(), inner_hash.begin(), inner_hash.end());
  return sha256(as_view(buffer));
}

std::string base64_encode(const std::vector<uint8_t>& data) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < data.size(); i += 3) {
    const std::size_t remaining = data.size() - i;
    const uint32_t byte0 = data[i];
    const uint32_t byte1 = remaining > 1 ? data[i + 1] : 0;
    const uint32_t byte2 = remaining > 2 ? data[i + 2] : 0;
    const uint32_t chunk = (byte0 << 16) | (byte1 << 8) | byte2;
    out.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
    out.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
    out.push_back(remaining > 1 ? kAlphabet[(chunk >> 6) & 0x3F] : '=');
    out.push_back(remaining > 2 ? kAlphabet[chunk & 0x3F] : '=');
  }
  return out;
}

std::vector<uint8_t> base64_decode(std::string_view text) {
  auto value_of = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };

  std::vector<uint8_t> out;
  uint32_t buffer = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    const int value = value_of(c);
    if (value < 0) throw std::runtime_error("invalid base64 input");
    buffer = (buffer << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
    }
  }
  return out;
}

std::string hex_encode(const std::vector<uint8_t>& data) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (uint8_t byte : data) {
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0F]);
  }
  return out;
}

std::vector<uint8_t> random_bytes(std::size_t count) {
  static std::mutex mutex;
  static std::mt19937_64 engine{std::random_device{}()};
  std::lock_guard<std::mutex> lock(mutex);
  std::vector<uint8_t> out(count);
  for (std::size_t i = 0; i < count; i += 8) {
    const uint64_t chunk = engine();
    for (std::size_t j = 0; j < 8 && i + j < count; ++j) {
      out[i + j] = static_cast<uint8_t>((chunk >> (j * 8)) & 0xFF);
    }
  }
  return out;
}

}  // namespace rc
