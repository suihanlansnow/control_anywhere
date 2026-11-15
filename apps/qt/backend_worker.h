#pragma once

#include <QImage>
#include <QObject>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

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
  // 线程安全：把本地文件加入主控端的待发送队列。
  void sendFile(const QString& path);
  // UI 线程调用：回应被控端弹窗的“允许/拒绝”。
  void answerConfirmation(bool allow);

 public slots:
  void startHost(const rc::SessionConfig& cfg);
  void startClient(const rc::SessionConfig& cfg);
  void stop();
  // 临时关闭/恢复远程键鼠注入；会话保持连接，仅影响输入。
  void setInputEnabled(bool enabled);
  // 主控端断线后是否自动重连。
  void setAutoReconnect(bool enabled);

 signals:
  void logMessage(const QString& tag, const QString& level, const QString& msg);
  void statusChanged(const QString& state);
  void progressUpdated(int frames, qlonglong bytes);
  // 主控端解码后的画面（BGRA），由后台线程发出，Qt 自动排队到 UI 线程。
  void frameReady(QImage image);
  void finished(int exitCode);
  // 有主控端请求连接，需要本机用户确认；UI 线程弹窗后调用 answerConfirmation。
  void confirmationRequested(qulonglong sessionId, const QString& peerId,
                             const QString& peerAlias);

 private:
  // 工作线程阻塞等待 UI 决定，超时或停止时按拒绝处理。
  bool requestConfirmation(uint64_t session_id, const std::string& peer_id,
                           const std::string& peer_alias);

  std::mutex runner_mutex_;
  std::unique_ptr<rc::HostRunner> host_;
  std::unique_ptr<rc::ClientRunner> client_;
  // 被控端在中继不可达时自动重试，stop() 置位后退出重试循环。
  std::atomic<bool> stopping_{false};
  // 重启被控会话后仍保持用户选择的输入开关状态。
  std::atomic<bool> input_enabled_{true};
  // 主控端断线自动重连，运行期可改。
  std::atomic<bool> auto_reconnect_{true};
  // 确认弹窗：工作线程在此条件变量上等待 UI 线程的决定。
  std::mutex confirm_mutex_;
  std::condition_variable confirm_cv_;
  bool confirm_pending_ = false;
  bool confirm_decision_ = false;
  std::atomic<int> confirm_timeout_ms_{30000};
};
