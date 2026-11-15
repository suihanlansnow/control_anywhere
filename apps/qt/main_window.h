#pragma once

#include <QImage>
#include <QMainWindow>
#include <QThread>

#include "backend_worker.h"

class QLabel;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QSlider;
class QCheckBox;
class QPushButton;
class QStackedWidget;
class QTextEdit;
class QFrame;
#include <QCloseEvent>
#include <QSystemTrayIcon>

class QMenu;

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 protected:
  void closeEvent(QCloseEvent* event) override;

 private slots:
  void onNavClicked(int index);
  void onStartHost();
  void onStopHost();
  void onStartClient();
  void onStopClient();
  void onSaveSettings();
  void onCopyDeviceId();
  void onCopyHostPassword();
  void onToggleHostPassword();
  void onRegenerateHostPassword();
  void onHostStatus(const QString& state);
  void onClientStatus(const QString& state);
  void onLogMsg(const QString& tag, const QString& level, const QString& msg);
  void onProgressUpdated(int frames, qlonglong bytes);
  void onFrameReady(QImage image);
  void onHostFinished(int rc);
  void onClientFinished(int rc);
  void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

 private:
  void buildUI();
  QWidget* buildSidebar();
  QWidget* buildConnectPanel();
  QWidget* buildDisplayPanel();
  QWidget* buildNetworkPanel();
  QWidget* buildPermissionPanel();
  QWidget* buildAboutPanel();
  QFrame* makeCard(const QString& title);
  QPushButton* makeNavButton(const QString& text, int iconIndex);
  void saveSettings();
  void loadSettings();
  void ensureDeviceIdentity();
  void autoStartHost();
  void updateStatusLabel();
  void initTrayIcon();

  QStackedWidget* panels_ = nullptr;
  QLabel* statusText_ = nullptr;
  QTextEdit* logView_ = nullptr;
  // 远程画面显示区（解码后的 BGRA 缩放上屏）。
  QWidget* videoView_ = nullptr;

  // 本机（被控端）身份
  QLineEdit* editDeviceId_ = nullptr;
  QLineEdit* editPassword_ = nullptr;      // 本机被控密码
  QPushButton* btnPasswordToggle_ = nullptr;
  QLabel* hostStatusLabel_ = nullptr;
  QPushButton* btnHostStart_ = nullptr;
  QPushButton* btnHostStop_ = nullptr;

  // 连接远程设备
  QLineEdit* editRelayHost_ = nullptr;
  QSpinBox* spinRelayPort_ = nullptr;
  QLineEdit* editTargetId_ = nullptr;
  QLineEdit* editConnectPassword_ = nullptr;
  QLabel* clientStatusLabel_ = nullptr;
  QPushButton* btnConnect_ = nullptr;
  QPushButton* btnDisconnect_ = nullptr;

  QComboBox* comboCodec_ = nullptr;
  QCheckBox* switchAutoBitrate_ = nullptr;

  QSlider* sliderTimeout_ = nullptr;
  QLabel* labelTimeoutVal_ = nullptr;
  QCheckBox* switchAutoReconnect_ = nullptr;
  QCheckBox* switchTcpNoDelay_ = nullptr;

  QCheckBox* switchKeyboard_ = nullptr;
  QCheckBox* switchMouse_ = nullptr;
  QCheckBox* switchClipboard_ = nullptr;
  QCheckBox* switchFileTransfer_ = nullptr;
  QCheckBox* switchAudio_ = nullptr;
  QCheckBox* switchMultiMonitor_ = nullptr;

  QLabel* badgeConn_ = nullptr;
  QLabel* badgeCapture_ = nullptr;
  QLabel* badgeCodec_ = nullptr;
  QLabel* badgeLoss_ = nullptr;

  // 被控端与主控端各占一个线程：HostRunner / ClientRunner 都是阻塞运行，
  // 共用线程会导致开启被控后无法再连接其他设备。
  QThread* hostThread_ = nullptr;
  BackendWorker* hostWorker_ = nullptr;
  QThread* clientThread_ = nullptr;
  BackendWorker* clientWorker_ = nullptr;

  QString hostState_;
  QString clientState_;

  QSystemTrayIcon* trayIcon_ = nullptr;
  QMenu* trayMenu_ = nullptr;
};
