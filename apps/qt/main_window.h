#pragma once

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

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 private slots:
  void onNavClicked(int index);
  void onStartHost();
  void onStartClient();
  void onStop();
  void onSaveSettings();
  void onLogMsg(const QString& tag, const QString& level, const QString& msg);
  void onStatusChanged(const QString& state);
  void onProgressUpdated(int frames, int bytes);
  void onWorkerFinished(int rc);

 private:
  void buildUI();
  QWidget* buildSidebar();
  QWidget* buildSecurityPanel();
  QWidget* buildDisplayPanel();
  QWidget* buildNetworkPanel();
  QWidget* buildPermissionPanel();
  QWidget* buildAboutPanel();
  QFrame* makeCard(const QString& title);
  QPushButton* makeNavButton(const QString& text, int iconIndex);

  QStackedWidget* panels_ = nullptr;
  QLabel* statusText_ = nullptr;
  QTextEdit* logView_ = nullptr;

  QLineEdit* editPassword_ = nullptr;
  QCheckBox* switchForcePwd_ = nullptr;
  QCheckBox* switchAes_ = nullptr;
  QCheckBox* switchCertVerify_ = nullptr;

  QComboBox* comboResolution_ = nullptr;
  QSlider* sliderFps_ = nullptr;
  QLabel* labelFpsVal_ = nullptr;
  QSlider* sliderQuality_ = nullptr;
  QLabel* labelQualityVal_ = nullptr;
  QComboBox* comboCodec_ = nullptr;
  QCheckBox* switchAutoBitrate_ = nullptr;

  QLineEdit* editRelayHost_ = nullptr;
  QSpinBox* spinRelayPort_ = nullptr;
  QLineEdit* editDeviceId_ = nullptr;
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

  QThread* workerThread_ = nullptr;
  BackendWorker* worker_ = nullptr;
};
