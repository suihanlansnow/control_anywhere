#include "base/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace rc {
namespace {

std::atomic<int>& level_storage() {
  static std::atomic<int> value{static_cast<int>(LogLevel::Info)};
  return value;
}

std::mutex& log_mutex() {
  static std::mutex m;
  return m;
}

const char* level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
  }
  return "INFO";
}

}  // namespace

void set_log_level(LogLevel level) {
  level_storage().store(static_cast<int>(level));
}

LogLevel log_level() {
  return static_cast<LogLevel>(level_storage().load());
}

void log_write(LogLevel level, std::string_view tag, std::string_view message) {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto millis =
      duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

  const std::time_t stamp = system_clock::to_time_t(now);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &stamp);
#else
  localtime_r(&stamp, &local);
#endif

  char ts[32];
  std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d", local.tm_hour,
                local.tm_min, local.tm_sec, static_cast<int>(millis));

  std::lock_guard<std::mutex> lock(log_mutex());
  std::fprintf(stderr, "%s %-5s [%.*s] %.*s\n", ts, level_name(level),
               static_cast<int>(tag.size()), tag.data(),
               static_cast<int>(message.size()), message.data());
  std::fflush(stderr);
}

}  // namespace rc
