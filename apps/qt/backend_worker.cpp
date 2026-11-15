#include "backend_worker.h"

#include <chrono>
#include <cstring>
#include <thread>

BackendWorker::BackendWorker(QObject* parent) : QObject(parent) {}
BackendWorker::~BackendWorker() { stop(); }

void BackendWorker::startHost(const rc::SessionConfig& cfg) {
  stopping_.store(false);
  // 中继未就绪时持续重试，保证被控端在无人值守时也能自动恢复。
  while (!stopping_.load()) {
    {
      std::lock_guard<std::mutex> lock(runner_mutex_);
      host_ = std::make_unique<rc::HostRunner>(cfg);
      host_->set_input_enabled(input_enabled_.load());
    }
    host_->set_log_callback([this](const std::string& tag, const std::string& level, const std::string& msg) {
      emit logMessage(QString::fromStdString(tag),
                      QString::fromStdString(level),
                      QString::fromStdString(msg));
    });
    host_->set_status_callback([this](const std::string& state) {
      emit statusChanged(QString::fromStdString(state));
    });
    host_->set_progress_callback([this](int64_t frames, int64_t bytes) {
      emit progressUpdated(static_cast<int>(frames),
                           static_cast<qlonglong>(bytes));
    });
    confirm_timeout_ms_.store(cfg.confirm_timeout_ms > 0 ? cfg.confirm_timeout_ms : 30000);
    host_->set_confirm_callback(
        [this](uint64_t session_id, const std::string& peer_id,
               const std::string& peer_alias) {
          return requestConfirmation(session_id, peer_id, peer_alias);
        });
    const int rc = host_->run();
    {
      std::lock_guard<std::mutex> lock(runner_mutex_);
      host_.reset();
    }
    if (rc == 0 || stopping_.load()) break;

    emit statusChanged(QStringLiteral("retrying"));
    for (int i = 0; i < 30 && !stopping_.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }
  emit finished(0);
}

void BackendWorker::startClient(const rc::SessionConfig& cfg) {
  stopping_.store(false);
  // 断线后按开关自动重连；认证失败（rc==3）重连也无意义，直接结束。
  while (!stopping_.load()) {
    {
      std::lock_guard<std::mutex> lock(runner_mutex_);
      client_ = std::make_unique<rc::ClientRunner>(cfg);
    }
    client_->set_log_callback([this](const std::string& tag, const std::string& level, const std::string& msg) {
      emit logMessage(QString::fromStdString(tag),
                      QString::fromStdString(level),
                      QString::fromStdString(msg));
    });
    client_->set_status_callback([this](const std::string& state) {
      emit statusChanged(QString::fromStdString(state));
    });
    client_->set_progress_callback([this](int64_t frames, int64_t bytes) {
      emit progressUpdated(static_cast<int>(frames),
                           static_cast<qlonglong>(bytes));
    });
    client_->set_frame_callback([this](const uint8_t* bgra, int width, int height, int stride) {
      QImage image(width, height, QImage::Format_ARGB32);
      if (image.isNull()) return;
      const std::size_t w4 = static_cast<std::size_t>(width) * 4;
      if (stride == static_cast<int>(w4)) {
        std::memcpy(image.bits(), bgra, w4 * static_cast<std::size_t>(height));
      } else {
        for (int y = 0; y < height; ++y) {
          std::memcpy(image.scanLine(y),
                      bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                      w4);
        }
      }
      emit frameReady(image);
    });
    const int rc = client_->run();
    {
      std::lock_guard<std::mutex> lock(runner_mutex_);
      client_.reset();
    }
    // 认证失败重连也无意义；其余退出（中继断开、被控端下线）都按开关重连。
    if (stopping_.load() || rc == 3) break;
    if (!auto_reconnect_.load()) break;

    emit logMessage(QStringLiteral("ctrl"), QStringLiteral("WARN"),
                    QStringLiteral("connection lost (rc=%1), retrying…").arg(rc));
    emit statusChanged(QStringLiteral("retrying"));
    for (int i = 0; i < 30 && !stopping_.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  emit finished(0);
}

void BackendWorker::sendInput(const rc::InputEvent& event) {
  std::lock_guard<std::mutex> lock(runner_mutex_);
  if (client_) client_->send_input(event);
}

void BackendWorker::sendFile(const QString& path) {
  if (path.isEmpty()) return;
  std::lock_guard<std::mutex> lock(runner_mutex_);
  if (client_) client_->queue_file(path.toStdString());
}

void BackendWorker::stop() {
  stopping_.store(true);
  // 唤醒可能正在等待用户确认的工作线程，按拒绝退出，避免 stop 卡到超时。
  {
    std::lock_guard<std::mutex> lock(confirm_mutex_);
    confirm_pending_ = false;
    confirm_decision_ = false;
  }
  confirm_cv_.notify_all();
  std::lock_guard<std::mutex> lock(runner_mutex_);
  if (host_) host_->stop();
  if (client_) client_->stop();
}

bool BackendWorker::requestConfirmation(uint64_t session_id, const std::string& peer_id,
                                        const std::string& peer_alias) {
  std::unique_lock<std::mutex> lock(confirm_mutex_);
  confirm_pending_ = true;
  confirm_decision_ = false;
  // 工作线程发出的信号会排队到 UI 线程，弹窗期间本线程在此等待。
  emit confirmationRequested(static_cast<qulonglong>(session_id),
                             QString::fromStdString(peer_id),
                             QString::fromStdString(peer_alias));
  const int timeout = confirm_timeout_ms_.load();
  const bool replied = confirm_cv_.wait_for(
      lock, std::chrono::milliseconds(timeout),
      [this] { return !confirm_pending_ || stopping_.load(); });
  const bool allowed = replied && confirm_decision_ && !stopping_.load();
  confirm_pending_ = false;
  return allowed;
}

void BackendWorker::answerConfirmation(bool allow) {
  {
    std::lock_guard<std::mutex> lock(confirm_mutex_);
    if (!confirm_pending_) return;
    confirm_decision_ = allow;
    confirm_pending_ = false;
  }
  confirm_cv_.notify_all();
}

void BackendWorker::setInputEnabled(bool enabled) {
  input_enabled_.store(enabled);
  std::lock_guard<std::mutex> lock(runner_mutex_);
  if (host_) host_->set_input_enabled(enabled);
}

void BackendWorker::setAutoReconnect(bool enabled) { auto_reconnect_.store(enabled); }
