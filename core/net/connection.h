#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "base/json.h"
#include "net/socket.h"
#include "protocol/protocol.h"
#include "protocol/video_payload.h"

namespace rc {

struct DecodedFrame {
  MsgType type = MsgType::Error;
  uint8_t flags = 0;
  uint32_t msg_id = 0;
  uint64_t session_id = 0;
  uint32_t seq = 0;
  std::vector<uint8_t> payload;
};

// 一条帧化 TCP 连接：负责组装头部、分配 msg_id、串行化发送。
class Connection {
 public:
  explicit Connection(TcpSocket socket) : socket_(std::move(socket)) {
    // 发送超时兜底：对端长期不读时，send 不能永久阻塞（否则 send_mutex_ 会连带
    // 卡死所有控制消息，连关闭会话都发不出去）。
    socket_.set_send_timeout_ms(10000);
  }

  bool alive() const { return socket_.valid(); }
  void close() { socket_.close(); }
  void set_recv_timeout_ms(int milliseconds) {
    socket_.set_recv_timeout_ms(milliseconds);
  }
  void set_send_timeout_ms(int milliseconds) {
    socket_.set_send_timeout_ms(milliseconds);
  }

  bool send(MsgType type, uint64_t session_id, const uint8_t* payload,
            std::size_t length, uint32_t seq = 0, uint8_t flags = 0);

  bool send_json(MsgType type, uint64_t session_id, const json::Value& value,
                 uint32_t seq = 0, uint8_t flags = 0);

  // 视频帧专用：在复用缓冲里直接写帧头 + 视频头 + 码流，一次发送，零中间分配。
  bool send_video_frame(uint64_t session_id, const VideoFrameMeta& meta,
                        const uint8_t* data, std::size_t length, uint32_t seq);

  bool recv(DecodedFrame& out);

  uint32_t next_msg_id() { return msg_id_.fetch_add(1); }

 private:
  // 把帧头写进 send_buf_ 前 24 字节；返回指向 payload 区的指针。
  uint8_t* write_frame_header(MsgType type, uint64_t session_id, uint32_t seq,
                              uint8_t flags, std::size_t payload_len);

  std::mutex send_mutex_;
  TcpSocket socket_;
  std::atomic<uint32_t> msg_id_{1};
  std::vector<uint8_t> send_buf_;  // 复用的发送缓冲，避免每帧堆分配
};

}  // namespace rc
