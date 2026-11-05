#include "backend_worker.h"

#include <QMetaObject>

#include <cstring>

BackendWorker::BackendWorker(QObject* parent) : QObject(parent) {}
BackendWorker::~BackendWorker() { stop(); }

void BackendWorker::startHost(const rc::SessionConfig& cfg) {
  host_ = std::make_unique<rc::HostRunner>(cfg);
  host_->set_log_callback([this](const std::string& tag, const std::string& level, const std::string& msg) {
    QMetaObject::invokeMethod(this, "logMessage", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(tag)),
                              Q_ARG(QString, QString::fromStdString(level)),
                              Q_ARG(QString, QString::fromStdString(msg)));
  });
  host_->set_status_callback([this](const std::string& state) {
    QMetaObject::invokeMethod(this, "statusChanged", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(state)));
  });
  host_->set_progress_callback([this](int64_t frames, int64_t bytes) {
    QMetaObject::invokeMethod(this, "progressUpdated", Qt::QueuedConnection,
                              Q_ARG(int, static_cast<int>(frames)),
                              Q_ARG(qlonglong, static_cast<qlonglong>(bytes)));
  });
  int rc = host_->run();
  host_.reset();
  emit finished(rc);
}

void BackendWorker::startClient(const rc::SessionConfig& cfg) {
  client_ = std::make_unique<rc::ClientRunner>(cfg);
  client_->set_log_callback([this](const std::string& tag, const std::string& level, const std::string& msg) {
    QMetaObject::invokeMethod(this, "logMessage", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(tag)),
                              Q_ARG(QString, QString::fromStdString(level)),
                              Q_ARG(QString, QString::fromStdString(msg)));
  });
  client_->set_status_callback([this](const std::string& state) {
    QMetaObject::invokeMethod(this, "statusChanged", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(state)));
  });
  client_->set_progress_callback([this](int64_t frames, int64_t bytes) {
    QMetaObject::invokeMethod(this, "progressUpdated", Qt::QueuedConnection,
                              Q_ARG(int, static_cast<int>(frames)),
                              Q_ARG(qlonglong, static_cast<qlonglong>(bytes)));
  });
  client_->set_frame_callback([this](const uint8_t* bgra, int width, int height, int stride) {
    // 后台线程里做一次深拷贝：QImage 隐式共享，按值跨线程投递是安全的。
    QImage image(width, height, QImage::Format_ARGB32);
    if (image.isNull()) return;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
      std::memcpy(image.scanLine(y),
                  bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                  row_bytes);
    }
    emit frameReady(image);
  });
  int rc = client_->run();
  client_.reset();
  emit finished(rc);
}

void BackendWorker::stop() {
  if (host_) host_->stop();
  if (client_) client_->stop();
}
