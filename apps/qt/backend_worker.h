#pragma once

#include <QImage>
#include <QObject>
#include <atomic>
#include <memory>
#include <mutex>

#include "session/runner.h"
#include "session/session_config.h"

// 跨线程 invokeMethod 需要该类型可被 Qt 元对象系统识别。
Q_DECLARE_METATYPE(rc::SessionConfig)

class BackendWorker : public QObject {
  Q_OBJECT

 public:
  explicit BackendWorker(QObject* parent = nullptr);
  ~BackendWorker() override;

  // 从 UI 线程直接调用，仅写入有界队列，不阻塞画面接收线程。
  void sendInput(const rc::InputEvent& event);

 public slots:
  void startHost(const rc::SessionConfig& cfg);
  void startClient(const rc::SessionConfig& cfg);
  void stop();
  // 临时关闭/恢复远程键鼠注入；会话保持连接，仅影响输入。
  void setInputEnabled(bool enabled);

 signals:
  void logMessage(const QString& tag, const QString& level, const QString& msg);
  void statusChanged(const QString& state);
  void progressUpdated(int frames, qlonglong bytes);
  // 主控端解码后的画面（BGRA），由后台线程发出，Qt 自动排队到 UI 线程。
  void frameReady(QImage image);
  void finished(int exitCode);

 private:
  std::mutex runner_mutex_;
  std::unique_ptr<rc::HostRunner> host_;
  std::unique_ptr<rc::ClientRunner> client_;
  // 被控端在中继不可达时自动重试，stop() 置位后退出重试循环。
  std::atomic<bool> stopping_{false};
  // 重启被控会话后仍保持用户选择的输入开关状态。
  std::atomic<bool> input_enabled_{true};
};
