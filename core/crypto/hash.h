#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rc {

std::vector<uint8_t> sha256(std::string_view data);
std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key,
                                 const std::vector<uint8_t>& data);

std::string base64_encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> base64_decode(std::string_view text);

std::string hex_encode(const std::vector<uint8_t>& data);

std::vector<uint8_t> random_bytes(std::size_t count);

}  // namespace rc
