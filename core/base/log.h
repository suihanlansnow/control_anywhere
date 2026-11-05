#pragma once

#include <sstream>
#include <string>
#include <string_view>

namespace rc {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_log_level(LogLevel level);
LogLevel log_level();
void log_write(LogLevel level, std::string_view tag, std::string_view message);

}  // namespace rc

#define RC_LOG_AT(level, tag, expr)                                      \
  do {                                                                   \
    if (static_cast<int>(level) >= static_cast<int>(rc::log_level())) {  \
      std::ostringstream _rc_log_os;                                     \
      _rc_log_os << expr;                                                \
      rc::log_write(level, tag, _rc_log_os.str());                       \
    }                                                                    \
  } while (false)

#define RC_LOGD(tag, expr) RC_LOG_AT(rc::LogLevel::Debug, tag, expr)
#define RC_LOGI(tag, expr) RC_LOG_AT(rc::LogLevel::Info, tag, expr)
#define RC_LOGW(tag, expr) RC_LOG_AT(rc::LogLevel::Warn, tag, expr)
#define RC_LOGE(tag, expr) RC_LOG_AT(rc::LogLevel::Error, tag, expr)
