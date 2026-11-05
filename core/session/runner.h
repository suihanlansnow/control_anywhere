#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "base/log.h"
#include "capture/capture.h"
#include "codec/encoder.h"
#include "codec/decoder.h"
#include "net/connection.h"
#include "net/socket.h"
#include "protocol/protocol.h"
#include "render/renderer.h"
#include "session/session_config.h"
#include "session/session.h"

namespace rc {

class HostRunner {
 public:
  explicit HostRunner(const SessionConfig& cfg);
  ~HostRunner();

  void set_log_callback(LogCallback cb) { log_cb_ = std::move(cb); }
  void set_status_callback(StatusCallback cb) { status_cb_ = std::move(cb); }
  void set_progress_callback(ProgressCallback cb) { progress_cb_ = std::move(cb); }

  int run();
  void stop();

 private:
  void log_(const std::string& level, const std::string& tag, const std::string& msg);
  int stream_frames_(Connection& conn, uint64_t session_id);

  SessionConfig cfg_;
  std::atomic<bool> stop_flag_{false};
  LogCallback log_cb_;
  StatusCallback status_cb_;
  ProgressCallback progress_cb_;
};

class ClientRunner {
 public:
  explicit ClientRunner(const SessionConfig& cfg);
  ~ClientRunner();

  void set_log_callback(LogCallback cb) { log_cb_ = std::move(cb); }
  void set_status_callback(StatusCallback cb) { status_cb_ = std::move(cb); }
  void set_progress_callback(ProgressCallback cb) { progress_cb_ = std::move(cb); }

  int run();
  void stop();

 private:
  void log_(const std::string& level, const std::string& tag, const std::string& msg);

  SessionConfig cfg_;
  std::atomic<bool> stop_flag_{false};
  LogCallback log_cb_;
  StatusCallback status_cb_;
  ProgressCallback progress_cb_;
};

}  // namespace rc
