#pragma once

#include <QImage>
#include <QObject>
#include <memory>

#include "session/runner.h"
#include "session/session_config.h"

class BackendWorker : public QObject {
  Q_OBJECT

 public:
  explicit BackendWorker(QObject* parent = nullptr);
  ~BackendWorker() override;

 public slots:
  void startHost(const rc::SessionConfig& cfg);
  void startClient(const rc::SessionConfig& cfg);
  void stop();

 signals:
  void logMessage(const QString& tag, const QString& level, const QString& msg);
  void statusChanged(const QString& state);
  void progressUpdated(int frames, qlonglong bytes);
  // 主控端解码后的画面（BGRA），由后台线程发出，Qt 自动排队到 UI 线程。
  void frameReady(QImage image);
  void finished(int exitCode);

 private:
  std::unique_ptr<rc::HostRunner> host_;
  std::unique_ptr<rc::ClientRunner> client_;
};
