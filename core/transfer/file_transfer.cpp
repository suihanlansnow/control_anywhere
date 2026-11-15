#include "transfer/file_transfer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace rc {
namespace {

namespace fs = std::filesystem;

std::string basename_of(const std::string& name) {
  const std::size_t pos = name.find_last_of("/\\");
  return pos == std::string::npos ? name : name.substr(pos + 1);
}

fs::path unique_path(const fs::path& dir, const std::string& name) {
  const fs::path first = dir / name;
  if (!fs::exists(first)) return first;
  const fs::path stem = first.stem();
  const fs::path ext = first.extension();
  for (int i = 1; i < 1000; ++i) {
    const fs::path candidate =
        dir / (stem.string() + " (" + std::to_string(i) + ")" + ext.string());
    if (!fs::exists(candidate)) return candidate;
  }
  return dir / (stem.string() + "-" + std::to_string(fs::file_size(first)) + ext.string());
}

}  // namespace

void write_u32_le(uint8_t* out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

void write_u64_le(uint8_t* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

uint32_t read_u32_le(const uint8_t* data) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[i]) << (8 * i);
  return value;
}

uint64_t read_u64_le(const uint8_t* data) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[i]) << (8 * i);
  return value;
}

std::string sanitize_file_name(const std::string& name) {
  std::string out = basename_of(name);
  for (char& ch : out) {
    const unsigned char value = static_cast<unsigned char>(ch);
    if (value < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '|' ||
        ch == '?' || ch == '*') {
      ch = '_';
    }
  }
  if (out.empty() || out == "." || out == "..") out = "received.bin";
  if (out.size() > 200) out = out.substr(0, 200);
  return out;
}

std::string ensure_receive_dir() {
  fs::path base;
  if (const char* home = std::getenv("USERPROFILE")) {
    base = home;
  } else if (const char* home = std::getenv("HOME")) {
    base = home;
  }
  fs::path dir = base.empty() ? fs::path("rc-recv") : base / "Downloads" / "rc-recv";
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return {};
  return dir.string();
}

bool FileSender::open(const std::string& path, std::string* error) {
  close();
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec) {
    if (error) *error = "cannot stat file: " + ec.message();
    return false;
  }
  in_.open(path, std::ios::binary);
  if (!in_) {
    if (error) *error = "cannot open file";
    return false;
  }
  name_ = sanitize_file_name(path);
  size_ = size;
  read_ = 0;
  return true;
}

void FileSender::close() {
  if (in_.is_open()) in_.close();
  name_.clear();
  size_ = 0;
  read_ = 0;
}

bool FileSender::read_chunk(std::vector<uint8_t>& data, std::string* error) {
  if (!in_.is_open()) {
    if (error) *error = "file not open";
    return false;
  }
  data.resize(kFileChunkDataBytes);
  in_.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  const std::streamsize got = in_.gcount();
  if (got <= 0) {
    data.clear();
    if (in_.eof()) return true;
    if (error) *error = "read failed";
    return false;
  }
  data.resize(static_cast<std::size_t>(got));
  read_ += static_cast<uint64_t>(got);
  return true;
}

bool FileReceiver::begin(const std::string& name, uint64_t size, std::string* error) {
  abort();
  const std::string dir = ensure_receive_dir();
  if (dir.empty()) {
    if (error) *error = "cannot create receive directory";
    return false;
  }
  const fs::path target = unique_path(fs::path(dir), sanitize_file_name(name));
  out_.open(target, std::ios::binary | std::ios::trunc);
  if (!out_) {
    if (error) *error = "cannot create file";
    return false;
  }
  path_ = target.string();
  expected_ = size;
  written_ = 0;
  return true;
}

bool FileReceiver::write(uint64_t offset, const uint8_t* data, std::size_t size,
                         std::string* error) {
  if (!out_.is_open()) {
    if (error) *error = "file not open";
    return false;
  }
  if (offset + size > expected_) {
    if (error) *error = "chunk exceeds declared size";
    return false;
  }
  out_.seekp(static_cast<std::streamoff>(offset));
  out_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  if (!out_.good()) {
    if (error) *error = "write failed";
    return false;
  }
  written_ = std::max(written_, offset + size);
  return true;
}

bool FileReceiver::finish(std::string* error) {
  if (!out_.is_open()) {
    if (error) *error = "file not open";
    return false;
  }
  out_.flush();
  out_.close();
  if (written_ != expected_) {
    if (error) {
      *error = "size mismatch: got " + std::to_string(written_) + " expected " +
               std::to_string(expected_);
    }
    return false;
  }
  return true;
}

void FileReceiver::abort() {
  if (out_.is_open()) out_.close();
  if (!path_.empty()) {
    std::error_code ec;
    fs::remove(path_, ec);
  }
  path_.clear();
  expected_ = 0;
  written_ = 0;
}

}  // namespace rc
