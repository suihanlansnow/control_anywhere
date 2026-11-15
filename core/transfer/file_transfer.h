#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace rc {

// FileChunk 载荷布局：transfer_id(u32) + offset(u64) + 原始字节。
constexpr std::size_t kFileChunkHeaderBytes = 12;
constexpr std::size_t kFileChunkDataBytes = 64 * 1024;

void write_u32_le(uint8_t* out, uint32_t value);
void write_u64_le(uint8_t* out, uint64_t value);
uint32_t read_u32_le(const uint8_t* data);
uint64_t read_u64_le(const uint8_t* data);

// 去掉路径分隔符与非法字符，防止对端借文件名写到接收目录之外。
std::string sanitize_file_name(const std::string& name);

// 返回接收目录并确保存在（Windows: %USERPROFILE%\Downloads\rc-recv，
// Linux: ~/Downloads/rc-recv）；无法创建时返回空串。
std::string ensure_receive_dir();

class FileSender {
 public:
  bool open(const std::string& path, std::string* error);
  void close();
  const std::string& name() const { return name_; }
  uint64_t size() const { return size_; }
  // 顺序读取下一块；data 为空表示已到文件末尾。
  bool read_chunk(std::vector<uint8_t>& data, std::string* error);

 private:
  std::ifstream in_;
  std::string name_;
  uint64_t size_ = 0;
  uint64_t read_ = 0;
};

class FileReceiver {
 public:
  // 在接收目录内创建唯一文件名并打开写入。
  bool begin(const std::string& name, uint64_t size, std::string* error);
  bool write(uint64_t offset, const uint8_t* data, std::size_t size, std::string* error);
  // 关闭文件并校验实际写入字节数与声明一致。
  bool finish(std::string* error);
  void abort();
  const std::string& path() const { return path_; }
  uint64_t expected_size() const { return expected_; }

 private:
  std::ofstream out_;
  std::string path_;
  uint64_t expected_ = 0;
  uint64_t written_ = 0;
};

}  // namespace rc
