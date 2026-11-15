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
  int rc = client_->run();
  {
    std::lock_guard<std::mutex> lock(runner_mutex_);
    client_.reset();
  }
  emit finished(rc);
}

void BackendWorker::sendInput(const rc::InputEvent& event) {
  std::lock_guard<std::mutex> lock(runner_mutex_);
  if (client_) client_->send_input(event);
}

void BackendWorker::stop() {
  stopping_.store(true);
  std::lock_guard<std::mutex> lock(runner_mutex_);
  if (host_) host_->stop();
  if (client_) client_->stop();
}
