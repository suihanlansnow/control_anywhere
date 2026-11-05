#include <QApplication>
#include "main_window.h"

#include <QAction>
#include <QMenu>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

namespace {

class VideoWidget : public QWidget {
 public:
  explicit VideoWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setMinimumHeight(320);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setStyleSheet("background-color:#101014; color:#8a8f98; border-radius:6px;");
  }

  void setFrame(const QImage& image) {
    frame_ = image;
    update();
  }

  void setPlaceholder(const QString& text) {
    placeholder_ = text;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.fillRect(rect(), QColor("#101014"));

    if (frame_.isNull()) {
      p.setPen(QColor("#8a8f98"));
      p.drawText(rect(), Qt::AlignCenter, placeholder_);
      return;
    }

    const QSize s = frame_.size().scaled(rect().size(), Qt::KeepAspectRatio);
    const int x = (width() - s.width()) / 2;
    const int y = (height() - s.height()) / 2;
    p.drawImage(QRect(x, y, s.width(), s.height()), frame_);
  }

 private:
  QImage frame_;
  QString placeholder_{tr("等待远程画面…")};
};

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(tr("远程控制 · 设置"));
  setObjectName("MainWindow");
  buildUI();

  workerThread_ = new QThread(this);
  worker_ = new BackendWorker();
  worker_->moveToThread(workerThread_);
  connect(workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
  connect(this, &MainWindow::destroyed, workerThread_, &QThread::quit);

  connect(worker_, &BackendWorker::logMessage, this, &MainWindow::onLogMsg);
  connect(worker_, &BackendWorker::statusChanged, this, &MainWindow::onStatusChanged);
  connect(worker_, &BackendWorker::progressUpdated, this, &MainWindow::onProgressUpdated);
  connect(worker_, &BackendWorker::frameReady, this, &MainWindow::onFrameReady);
  connect(worker_, &BackendWorker::finished, this, &MainWindow::onWorkerFinished);

  workerThread_->start();

  loadSettings();
  initTrayIcon();
}

MainWindow::~MainWindow() {
  saveSettings();
  if (workerThread_) {
    worker_->stop();
    workerThread_->quit();
    workerThread_->wait(2000);
  }
}

void MainWindow::closeEvent(QCloseEvent* event) {
  saveSettings();
  if (trayIcon_ && trayIcon_->isVisible()) {
    hide();
    event->ignore();
  } else {
    event->accept();
  }
}

QPushButton* MainWindow::makeNavButton(const QString& text, int /*iconIndex*/) {
  auto* btn = new QPushButton(text);
  btn->setObjectName("NavButton");
  btn->setCheckable(true);
  btn->setCursor(Qt::PointingHandCursor);
  btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  btn->setMinimumHeight(36);
  return btn;
}

void MainWindow::buildUI() {
  auto* central = new QWidget(this);
  auto* root = new QHBoxLayout(central);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  root->addWidget(buildSidebar());

  panels_ = new QStackedWidget();
  panels_->addWidget(buildSecurityPanel());
  panels_->addWidget(buildDisplayPanel());
  panels_->addWidget(buildNetworkPanel());
  panels_->addWidget(buildPermissionPanel());
  panels_->addWidget(buildAboutPanel());
  panels_->setObjectName("MainContent");
  root->addWidget(panels_, 1);

  setCentralWidget(central);
  onNavClicked(0);
}

QWidget* MainWindow::buildSidebar() {
  auto* sidebar = new QFrame();
  sidebar->setObjectName("Sidebar");
  sidebar->setFixedWidth(230);

  auto* layout = new QVBoxLayout(sidebar);
  layout->setContentsMargins(14, 18, 14, 12);
  layout->setSpacing(0);

  auto* brand = new QFrame();
  auto* brandLayout = new QHBoxLayout(brand);
  brandLayout->setContentsMargins(8, 4, 8, 14);
  brandLayout->setSpacing(10);

  auto* logo = new QLabel("🖥");
  logo->setObjectName("BrandLogo");
  logo->setAlignment(Qt::AlignCenter);
  logo->setFixedSize(34, 34);
  brandLayout->addWidget(logo);

  auto* brandText = new QWidget();
  auto* brandTextLayout = new QVBoxLayout(brandText);
  brandTextLayout->setContentsMargins(0, 0, 0, 0);
  brandTextLayout->setSpacing(1);
  auto* bt1 = new QLabel(tr("远程控制"));
  bt1->setObjectName("BrandTitle");
  auto* bt2 = new QLabel(tr("Remote Control"));
  bt2->setObjectName("BrandSub");
  brandTextLayout->addWidget(bt1);
  brandTextLayout->addWidget(bt2);
  brandLayout->addWidget(brandText);
  brandLayout->addStretch();
  layout->addWidget(brand);

  auto* navFrame = new QFrame();
  navFrame->setObjectName("NavList");
  auto* navLayout = new QVBoxLayout(navFrame);
  navLayout->setContentsMargins(4, 14, 4, 0);
  navLayout->setSpacing(2);

  struct NavItem { QString label; int idx; };
  QList<NavItem> items = {
    {tr("安全"), 0}, {tr("显示"), 1}, {tr("网络"), 2}, {tr("权限"), 3}, {tr("关于"), 4}
  };
  QButtonGroup* group = new QButtonGroup(this);
  group->setExclusive(true);
  for (const auto& item : items) {
    auto* btn = makeNavButton(item.label, item.idx);
    btn->setProperty("index", item.idx);
    group->addButton(btn, item.idx);
    navLayout->addWidget(btn);
  }
  layout->addWidget(navFrame);
  layout->addStretch();

  auto* footer = new QFrame();
  footer->setObjectName("SidebarFooter");
  auto* footerLayout = new QHBoxLayout(footer);
  footerLayout->setContentsMargins(8, 10, 8, 4);
  footerLayout->setSpacing(8);
  auto* dot = new QLabel();
  dot->setObjectName("StatusDot");
  dot->setFixedSize(10, 10);
  statusText_ = new QLabel(tr("未连接"));
  footerLayout->addWidget(dot);
  footerLayout->addWidget(statusText_);
  footerLayout->addStretch();
  layout->addWidget(footer);

  connect(group, &QButtonGroup::idClicked, this, &MainWindow::onNavClicked);
  static_cast<QPushButton*>(group->button(0))->setChecked(true);

  return sidebar;
}

void MainWindow::onNavClicked(int index) {
  if (panels_) panels_->setCurrentIndex(index);
}

QFrame* MainWindow::makeCard(const QString& title) {
  auto* card = new QFrame();
  card->setObjectName("Card");
  card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  auto* vbox = new QVBoxLayout(card);
  vbox->setContentsMargins(20, 18, 20, 18);
  vbox->setSpacing(12);

  auto* titleRow = new QHBoxLayout();
  titleRow->setSpacing(8);
  auto* bar = new QFrame();
  bar->setObjectName("CardTitleBar");
  bar->setFixedWidth(3);
  bar->setFixedHeight(14);
  titleRow->addWidget(bar);
  auto* t = new QLabel(title);
  t->setObjectName("CardTitle");
  titleRow->addWidget(t);
  titleRow->addStretch();
  vbox->addLayout(titleRow);

  return card;
}

QWidget* MainWindow::buildSecurityPanel() {
  auto* panel = new QWidget();
  auto* scroll = new QVBoxLayout(panel);
  scroll->setContentsMargins(32, 24, 32, 24);
  scroll->setSpacing(16);

  auto* header = new QWidget();
  auto* hl = new QVBoxLayout(header);
  hl->setContentsMargins(0, 0, 0, 4);
  hl->setSpacing(4);
  auto* title = new QLabel(tr("安全"));
  title->setObjectName("PanelTitle");
  auto* desc = new QLabel(tr("配置连接认证与加密，保护远程会话安全。"));
  desc->setObjectName("PanelDesc");
  desc->setWordWrap(true);
  hl->addWidget(title);
  hl->addWidget(desc);
  scroll->addWidget(header);

  auto* card1 = makeCard(tr("连接认证"));
  {
    auto* vbox = static_cast<QVBoxLayout*>(card1->layout());
    auto* lbl1 = new QLabel(tr("连接密码"));
    lbl1->setObjectName("FieldLabel");
    vbox->addWidget(lbl1);
    editPassword_ = new QLineEdit();
    editPassword_->setEchoMode(QLineEdit::Password);
    editPassword_->setPlaceholderText(tr("输入连接密码"));
    vbox->addWidget(editPassword_);
    auto* hint = new QLabel(tr("长度不少于 6 位，建议包含字母与数字组合。"));
    hint->setObjectName("FieldHint");
    vbox->addWidget(hint);

    vbox->addSpacing(8);
    switchForcePwd_ = new QCheckBox(tr("强制密码保护"));
    switchForcePwd_->setObjectName("Switch");
    switchForcePwd_->setChecked(true);
    auto* hint2 = new QLabel(tr("未经授权的连接将被拒绝"));
    hint2->setObjectName("FieldHint");
    auto* row = new QVBoxLayout();
    row->setSpacing(2);
    row->addWidget(switchForcePwd_);
    row->addWidget(hint2);
    vbox->addLayout(row);
  }
  scroll->addWidget(card1);

  auto* card2 = makeCard(tr("传输加密"));
  {
    auto* vbox = static_cast<QVBoxLayout*>(card2->layout());
    switchAes_ = new QCheckBox(tr("AES-256 加密"));
    switchAes_->setObjectName("Switch");
    switchAes_->setChecked(true);
    auto* h1 = new QLabel(tr("所有帧数据与控制指令均加密传输"));
    h1->setObjectName("FieldHint");
    auto* row1 = new QVBoxLayout();
    row1->setSpacing(2);
    row1->addWidget(switchAes_);
    row1->addWidget(h1);
    vbox->addLayout(row1);

    auto* line = new QFrame();
    line->setFrameShape(QFrame::HLine);
    vbox->addWidget(line);

    switchCertVerify_ = new QCheckBox(tr("证书验证"));
    switchCertVerify_->setObjectName("Switch");
    auto* h2 = new QLabel(tr("中继服务器证书校验（防止中间人攻击）"));
    h2->setObjectName("FieldHint");
    auto* row2 = new QVBoxLayout();
    row2->setSpacing(2);
    row2->addWidget(switchCertVerify_);
    row2->addWidget(h2);
    vbox->addLayout(row2);
  }
  scroll->addWidget(card2);

  auto* btnRow = new QHBoxLayout();
  btnRow->setSpacing(10);
  auto* saveBtn = new QPushButton(tr("保存"));
  saveBtn->setObjectName("BtnPrimary");
  auto* resetBtn = new QPushButton(tr("重置"));
  resetBtn->setObjectName("BtnGhost");
  btnRow->addWidget(saveBtn);
  btnRow->addWidget(resetBtn);
  btnRow->addStretch();
  scroll->addLayout(btnRow);
  scroll->addStretch();

  connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveSettings);
  return panel;
}

QWidget* MainWindow::buildDisplayPanel() {
  auto* panel = new QWidget();
  auto* scroll = new QVBoxLayout(panel);
  scroll->setContentsMargins(32, 24, 32, 24);
  scroll->setSpacing(16);

  auto* header = new QWidget();
  auto* hl = new QVBoxLayout(header);
  hl->setContentsMargins(0, 0, 0, 4);
  hl->setSpacing(4);
  auto* title = new QLabel(tr("显示"));
  title->setObjectName("PanelTitle");
  auto* desc = new QLabel(tr("调整画面分辨率、帧率与编码质量。"));
  desc->setObjectName("PanelDesc");
  desc->setWordWrap(true);
  hl->addWidget(title);
  hl->addWidget(desc);
  scroll->addWidget(header);

  auto* cardVideo = makeCard(tr("远程画面"));
  cardVideo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  {
    auto* vbox = static_cast<QVBoxLayout*>(cardVideo->layout());
    videoView_ = new VideoWidget();
    vbox->addWidget(videoView_, 1);
  }
  scroll->addWidget(cardVideo, 1);

  auto* card1 = makeCard(tr("画面质量"));
  {
    auto* vbox = static_cast<QVBoxLayout*>(card1->layout());
    auto* lbl1 = new QLabel(tr("分辨率"));
    lbl1->setObjectName("FieldLabel");
    vbox->addWidget(lbl1);
    comboResolution_ = new QComboBox();
    comboResolution_->addItem("1920 × 1080", "1920x1080");
    comboResolution_->addItem("2560 × 1440", "2560x1440");
    comboResolution_->addItem("3840 × 2160", "3840x2160");
    comboResolution_->addItem("1280 × 720", "1280x720");
    comboResolution_->setCurrentIndex(0);
    vbox->addWidget(comboResolution_);

    auto* lbl2 = new QLabel(tr("帧率"));
    lbl2->setObjectName("FieldLabel");
    vbox->addWidget(lbl2);
    auto* fpsRow = new QHBoxLayout();
    fpsRow->setSpacing(12);
    sliderFps_ = new QSlider(Qt::Horizontal);
    sliderFps_->setRange(15, 120);
    sliderFps_->setValue(60);
    labelFpsVal_ = new QLabel("60 fps");
    labelFpsVal_->setMinimumWidth(60);
    labelFpsVal_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    labelFpsVal_->setStyleSheet("font-weight:600; color:#1f7a45;");
    fpsRow->addWidget(sliderFps_, 1);
    fpsRow->addWidget(labelFpsVal_);
    vbox->addLayout(fpsRow);

    auto* lbl3 = new QLabel(tr("编码质量"));
    lbl3->setObjectName("FieldLabel");
    vbox->addWidget(lbl3);
    auto* qRow = new QHBoxLayout();
    qRow->setSpacing(12);
    sliderQuality_ = new QSlider(Qt::Horizontal);
    sliderQuality_->setRange(1, 100);
    sliderQuality_->setValue(80);
    labelQualityVal_ = new QLabel("80");
    labelQualityVal_->setMinimumWidth(44);
    labelQualityVal_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    labelQualityVal_->setStyleSheet("font-weight:600; color:#1f7a45;");
    qRow->addWidget(sliderQuality_, 1);
    qRow->addWidget(labelQualityVal_);
    vbox->addLayout(qRow);

    connect(sliderFps_, &QSlider::valueChanged, this, [this](int v) {
      labelFpsVal_->setText(QString("%1 fps").arg(v));
    });
    connect(sliderQuality_, &QSlider::valueChanged, this, [this](int v) {
      labelQualityVal_->setText(QString::number(v));
    });
  }
  scroll->addWidget(card1);

  auto* card2 = makeCard(tr("编码与渲染"));
  {
    auto* vbox = static_cast<QVBoxLayout*>(card2->layout());
    auto* comboRow = new QHBoxLayout();
    comboRow->setSpacing(14);
    auto* col1 = new QVBoxLayout();
    auto* lblCodec = new QLabel(tr("视频编码"));
    lblCodec->setObjectName("FieldLabel");
    comboCodec_ = new QComboBox();
    comboCodec_->addItem(tr("H.264（硬件加速）"), "h264");
    comboCodec_->addItem(tr("H.264（软件编码）"), "h264-soft");
    comboCodec_->addItem(tr("JPEG"), "jpeg");
    col1->addWidget(lblCodec);
    col1->addWidget(comboCodec_);

    auto* col2 = new QVBoxLayout();
    auto* lblRender = new QLabel(tr("渲染模式"));
    lblRender->setObjectName("FieldLabel");
    auto* comboRender = new QComboBox();
    comboRender->addItem(tr("GPU 直出（零拷贝）"));
    comboRender->addItem(tr("CPU 回读"));
    col2->addWidget(lblRender);
    col2->addWidget(comboRender);
    comboRow->addLayout(col1, 1);
    comboRow->addLayout(col2, 1);
    vbox->addLayout(comboRow);

    vbox->addSpacing(8);
    switchAutoBitrate_ = new QCheckBox(tr("自适应码率"));
    switchAutoBitrate_->setObjectName("Switch");
    switchAutoBitrate_->setChecked(true);
    auto* h = new QLabel(tr("网络波动时自动降低码率保证流畅"));
    h->setObjectName("FieldHint");
    auto* tRow = new QVBoxLayout();
    tRow->setSpacing(2);
    tRow->addWidget(switchAutoBitrate_);
    tRow->addWidget(h);
    vbox->addLayout(tRow);
  }
  scroll->addWidget(card2);

  auto* btnRow = new QHBoxLayout();
  btnRow->setSpacing(10);
  auto* saveBtn = new QPushButton(tr("保存"));
  saveBtn->setObjectName("BtnPrimary");
  auto* resetBtn = new QPushButton(tr("重置"));
  resetBtn->setObjectName("BtnGhost");
  btnRow->addWidget(saveBtn);
  btnRow->addWidget(resetBtn);
  btnRow->addStretch();
  scroll->addLayout(btnRow);
  scroll->addStretch();

  connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveSettings);
  return panel;
}

QWidget* MainWindow::buildNetworkPanel() {
  auto* panel = new QWidget();
  auto* scroll = new QVBoxLayout(panel);
  scroll->setContentsMargins(32, 24, 32, 24);
  scroll->setSpacing(16);

  auto* header = new QWidget();
  auto* hl = new QVBoxLayout(header);
  hl->setContentsMargins(0, 0, 0, 4);
  hl->setSpacing(4);
  auto* title = new QLabel(tr("网络"));
  title->setObjectName("PanelTitle");
  auto* desc = new QLabel(tr("配置中继服务器与连接参数。"));
  desc->setObjectName("PanelDesc");
  desc->setWordWrap(true);
  hl->addWidget(title);
  hl->addWidget(desc);
  scroll->addWidget(header);

  auto* card1 = makeCard(tr("中继服务器"));
  {
    auto* vbox = static_cast<QVBoxLayout*>(card1->layout());
    auto* hostRow = new QHBoxLayout();
    hostRow->setSpacing(14);
    auto* hostCol = new QVBoxLayout();
    auto* lblHost = new QLabel(tr("服务器地址"));
    lblHost->setObjectName("FieldLabel");
    editRelayHost_ = new QLineEdit("127.0.0.1");
    hostCol->addWidget(lblHost);
    hostCol->addWidget(editRelayHost_);

    auto* portCol = new QVBoxLayout();
    auto* lblPort = new QLabel(tr("端口"));
    lblPort->setObjectName("FieldLabel");
    spinRelayPort_ = new QSpinBox();
    spinRelayPort_->setRange(1, 65535);
    spinRelayPort_->setValue(21116);
    portCol->addWidget(lblPort);
    portCol->addWidget(spinRelayPort_);
    spinRelayPort_->setMaximumWidth(110);
    hostRow->addLayout(hostCol, 1);
    hostRow->addLayout(portCol);
    vbox->addLayout(hostRow);

    auto* lblSid = new QLabel(tr("设备 ID"));
    lblSid->setObjectName("FieldLabel");
    editDeviceId_ = new QLineEdit();
    editDeviceId_->setPlaceholderText(tr("自动生成"));
    vbox->addWidget(lblSid);
    vbox->addWidget(editDeviceId_);
  }
  scroll->addWidget(card1);

  auto* card2 = makeCard(tr("连接参数"));
  {
    auto* vbox = static_cast<QVBoxLayout*>(card2->layout());
    auto* lbl = new QLabel(tr("连接超时（秒）"));
    lbl->setObjectName("FieldLabel");
    vbox->addWidget(lbl);
    auto* tRow = new QHBoxLayout();
    tRow->setSpacing(12);
    sliderTimeout_ = new QSlider(Qt::Horizontal);
    sliderTimeout_->setRange(3, 30);
    sliderTimeout_->setValue(10);
    labelTimeoutVal_ = new QLabel("10s");
    labelTimeoutVal_->setMinimumWidth(44);
    labelTimeoutVal_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    labelTimeoutVal_->setStyleSheet("font-weight:600; color:#1f7a45;");
    tRow->addWidget(sliderTimeout_, 1);
    tRow->addWidget(labelTimeoutVal_);
    vbox->addLayout(tRow);

    connect(sliderTimeout_, &QSlider::valueChanged, this, [this](int v) {
      labelTimeoutVal_->setText(QString("%1s").arg(v));
    });

    vbox->addSpacing(8);
    auto* line = new QFrame();
    line->setFrameShape(QFrame::HLine);
    vbox->addWidget(line);

    switchAutoReconnect_ = new QCheckBox(tr("自动重连"));
    switchAutoReconnect_->setObjectName("Switch");
    switchAutoReconnect_->setChecked(true);
    auto* h1 = new QLabel(tr("断开后自动尝试重新连接"));
    h1->setObjectName("FieldHint");
    auto* r1 = new QVBoxLayout();
    r1->setSpacing(2);
    r1->addWidget(switchAutoReconnect_);
    r1->addWidget(h1);
    vbox->addLayout(r1);

    auto* line2 = new QFrame();
    line2->setFrameShape(QFrame::HLine);
    vbox->addWidget(line2);

    switchTcpNoDelay_ = new QCheckBox(tr("TCP_NODELAY"));
    switchTcpNoDelay_->setObjectName("Switch");
    switchTcpNoDelay_->setChecked(true);
    auto* h2 = new QLabel(tr("禁用 Nagle 算法，降低交互延迟"));
    h2->setObjectName("FieldHint");
    auto* r2 = new QVBoxLayout();
    r2->setSpacing(2);
    r2->addWidget(switchTcpNoDelay_);
    r2->addWidget(h2);
    vbox->addLayout(r2);
  }
  scroll->addWidget(card2);

  auto* btnRow = new QHBoxLayout();
  btnRow->setSpacing(10);
  auto* hostBtn = new QPushButton(tr("启动被控端（Host）"));
  hostBtn->setObjectName("BtnPrimary");
  auto* clientBtn = new QPushButton(tr("连接主控（Client）"));
  clientBtn->setObjectName("BtnPrimary");
  auto* stopBtn = new QPushButton(tr("停止"));
  stopBtn->setObjectName("BtnGhost");
  btnRow->addWidget(hostBtn);
  btnRow->addWidget(clientBtn);
  btnRow->addWidget(stopBtn);
  btnRow->addStretch();
  scroll->addLayout(btnRow);

  logView_ = new QTextEdit();
  logView_->setReadOnly(true);
  logView_->setFixedHeight(140);
  logView_->setStyleSheet(
    "QTextEdit { background:#1f2933; color:#e0e0e0; border:1px solid #e4e8ed; border-radius:7px; padding:8px; font-family:Menlo,Consolas,monospace; font-size:12px; }"
  );
  scroll->addWidget(logView_);
  scroll->addStretch();

  connect(hostBtn, &QPushButton::clicked, this, &MainWindow::onStartHost);
  connect(clientBtn, &QPushButton::clicked, this, &MainWindow::onStartClient);
  connect(stopBtn, &QPushButton::clicked, this, &MainWindow::onStop);

  return panel;
}

QWidget* MainWindow::buildPermissionPanel() {
  auto* panel = new QWidget();
  auto* scroll = new QVBoxLayout(panel);
  scroll->setContentsMargins(32, 24, 32, 24);
  scroll->setSpacing(16);

  auto* header = new QWidget();
  auto* hl = new QVBoxLayout(header);
  hl->setContentsMargins(0, 0, 0, 4);
  hl->setSpacing(4);
  auto* title = new QLabel(tr("权限"));
  title->setObjectName("PanelTitle");
  auto* desc = new QLabel(tr("控制远程端可执行的操作范围。"));
  desc->setObjectName("PanelDesc");
  desc->setWordWrap(true);
  hl->addWidget(title);
  hl->addWidget(desc);
  scroll->addWidget(header);

  auto* card1 = makeCard(tr("输入控制"));
  {
    auto* vbox = static_cast<QVBoxLayout*>(card1->layout());

    auto makeToggle = [&](const QString& label, const QString& hint, bool checked) -> QCheckBox* {
      auto* cb = new QCheckBox(label);
      cb->setObjectName("Switch");
      cb->setChecked(checked);
      auto* h = new QLabel(hint);
      h->setObjectName("FieldHint");
      auto* row = new QVBoxLayout();
      row->setSpacing(2);
      row->addWidget(cb);
      row->addWidget(h);
      vbox->addLayout(row);
      return cb;
    };

    switchKeyboard_ = makeToggle(tr("键盘输入"), tr("允许远程端发送键盘事件"), true);
    auto* line = new QFrame(); line->setFrameShape(QFrame::HLine); vbox->addWidget(line);
    switchMouse_ = makeToggle(tr("鼠标控制"), tr("允许远程端移动鼠标与点击"), true);
    line = new QFrame(); line->setFrameShape(QFrame::HLine); vbox->addWidget(line);
    switchClipboard_ = makeToggle(tr("剪贴板同步"), tr("双向同步剪贴板内容"), false);
  }
  scroll->addWidget(card1);

  auto* card2 = makeCard(tr("系统权限"));
  {
    auto* vbox = static_cast<QVBoxLayout*>(card2->layout());

    auto makeToggle = [&](const QString& label, const QString& hint, bool checked) -> QCheckBox* {
      auto* cb = new QCheckBox(label);
      cb->setObjectName("Switch");
      cb->setChecked(checked);
      auto* h = new QLabel(hint);
      h->setObjectName("FieldHint");
      auto* row = new QVBoxLayout();
      row->setSpacing(2);
      row->addWidget(cb);
      row->addWidget(h);
      vbox->addLayout(row);
      return cb;
    };

    switchFileTransfer_ = makeToggle(tr("文件传输"), tr("允许在两端间传输文件"), false);
    auto* line = new QFrame(); line->setFrameShape(QFrame::HLine); vbox->addWidget(line);
    switchAudio_ = makeToggle(tr("音频传输"), tr("传输远程端音频输出"), false);
    line = new QFrame(); line->setFrameShape(QFrame::HLine); vbox->addWidget(line);
    switchMultiMonitor_ = makeToggle(tr("多显示器"), tr("允许切换远程显示器"), true);
  }
  scroll->addWidget(card2);

  auto* btnRow = new QHBoxLayout();
  btnRow->setSpacing(10);
  auto* saveBtn = new QPushButton(tr("保存"));
  saveBtn->setObjectName("BtnPrimary");
  auto* resetBtn = new QPushButton(tr("重置"));
  resetBtn->setObjectName("BtnGhost");
  btnRow->addWidget(saveBtn);
  btnRow->addWidget(resetBtn);
  btnRow->addStretch();
  scroll->addLayout(btnRow);
  scroll->addStretch();

  connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveSettings);
  return panel;
}

static QWidget* makeInfoRow(const QString& name, QWidget* value) {
  auto* row = new QHBoxLayout();
  row->setContentsMargins(0, 10, 0, 10);
  row->setSpacing(0);
  auto* n = new QLabel(name);
  n->setObjectName("InfoRowName");
  row->addWidget(n);
  row->addStretch();
  row->addWidget(value);
  auto* w = new QWidget();
  w->setLayout(row);
  auto* line = new QFrame();
  line->setFrameShape(QFrame::HLine);
  line->setStyleSheet("background:#e4e8ed; max-height:1px;");
  auto* parent = new QVBoxLayout();
  parent->setContentsMargins(0, 0, 0, 0);
  parent->setSpacing(0);
  parent->addWidget(w);
  parent->addWidget(line);
  auto* container = new QWidget();
  container->setLayout(parent);
  return container;
}

QWidget* MainWindow::buildAboutPanel() {
  auto* panel = new QWidget();
  auto* scroll = new QVBoxLayout(panel);
  scroll->setContentsMargins(32, 20, 32, 24);
  scroll->setSpacing(16);

  auto* hero = new QWidget();
  auto* hl = new QVBoxLayout(hero);
  hl->setContentsMargins(0, 20, 0, 10);
  hl->setSpacing(6);
  auto* logo = new QLabel("🖥");
  logo->setObjectName("AboutLogo");
  logo->setAlignment(Qt::AlignCenter);
  logo->setFixedSize(64, 64);
  auto* name = new QLabel(tr("远程控制"));
  name->setObjectName("AboutName");
  name->setAlignment(Qt::AlignCenter);
  auto* ver = new QLabel(tr("版本 0.1.0"));
  ver->setObjectName("AboutVer");
  ver->setAlignment(Qt::AlignCenter);
  hl->addWidget(logo, 0, Qt::AlignCenter);
  hl->addWidget(name);
  hl->addWidget(ver);
  scroll->addWidget(hero);

  auto* card1 = makeCard(tr("系统信息"));
  {
    auto* vbox = static_cast<QVBoxLayout*>(card1->layout());

    auto mkVal = [](const QString& text) -> QLabel* {
      auto* l = new QLabel(text);
      l->setObjectName("InfoRowValue");
      return l;
    };
    vbox->addWidget(makeInfoRow(tr("版本"), mkVal("0.1.0")));
    vbox->addWidget(makeInfoRow(tr("协议"), mkVal("rc-frame v1")));
    vbox->addWidget(makeInfoRow(tr("编码器"), mkVal("H.264 / JPEG")));
#ifdef __linux__
    vbox->addWidget(makeInfoRow(tr("平台"), mkVal("Linux")));
#else
    vbox->addWidget(makeInfoRow(tr("平台"), mkVal("Windows / Linux")));
#endif
    vbox->addWidget(makeInfoRow(tr("构建类型"), mkVal("Release")));
  }
  scroll->addWidget(card1);

  auto* card2 = makeCard(tr("运行状态"));
  {
    auto* vbox = static_cast<QVBoxLayout*>(card2->layout());

    auto mkBadge = [](const QString& text, const QString& objName) -> QLabel* {
      auto* l = new QLabel(text);
      l->setObjectName(objName);
      return l;
    };

    badgeConn_ = mkBadge(tr("● 未连接"), "BadgeOff");
    badgeCapture_ = mkBadge(tr("—"), "BadgeOff");
    badgeCodec_ = mkBadge(tr("—"), "BadgeOff");
    badgeLoss_ = mkBadge(tr("—"), "BadgeOff");

    vbox->addWidget(makeInfoRow(tr("连接状态"), badgeConn_));
    vbox->addWidget(makeInfoRow(tr("采集"), badgeCapture_));
    vbox->addWidget(makeInfoRow(tr("编码"), badgeCodec_));
    vbox->addWidget(makeInfoRow(tr("丢帧率"), badgeLoss_));
  }
  scroll->addWidget(card2);

  auto* btnRow = new QHBoxLayout();
  btnRow->setSpacing(10);
  auto* checkBtn = new QPushButton(tr("检查更新"));
  checkBtn->setObjectName("BtnGhost");
  auto* disconnectBtn = new QPushButton(tr("断开连接"));
  disconnectBtn->setObjectName("BtnDanger");
  btnRow->addWidget(checkBtn);
  btnRow->addStretch();
  btnRow->addWidget(disconnectBtn);
  scroll->addLayout(btnRow);
  scroll->addStretch();

  connect(disconnectBtn, &QPushButton::clicked, this, &MainWindow::onStop);
  return panel;
}

void MainWindow::onSaveSettings() {
  saveSettings();
  if (logView_) logView_->append(tr("设置已保存"));
}

void MainWindow::onStartHost() {
  rc::SessionConfig cfg;
  cfg.relay_host = editRelayHost_ ? editRelayHost_->text().toStdString() : "127.0.0.1";
  cfg.relay_port = spinRelayPort_ ? static_cast<uint16_t>(spinRelayPort_->value()) : 21116;
  cfg.device_id = editDeviceId_ ? editDeviceId_->text().toStdString() : "linux-host";
  cfg.password = editPassword_ ? editPassword_->text().toStdString() : "";
  cfg.codec = comboCodec_ ? comboCodec_->currentData().toString().toStdString() : "h264";
  cfg.fps = sliderFps_ ? sliderFps_->value() : 60;
  cfg.quality = sliderQuality_ ? sliderQuality_->value() : 75;
  cfg.capture = "x11";

  QMetaObject::invokeMethod(worker_, "startHost", Qt::QueuedConnection,
                            Q_ARG(rc::SessionConfig, cfg));
}

void MainWindow::onStartClient() {
  rc::SessionConfig cfg;
  cfg.relay_host = editRelayHost_ ? editRelayHost_->text().toStdString() : "127.0.0.1";
  cfg.relay_port = spinRelayPort_ ? static_cast<uint16_t>(spinRelayPort_->value()) : 21116;
  cfg.device_id = "linux-client";
  cfg.target_id = editDeviceId_ ? editDeviceId_->text().toStdString() : "linux-host";
  cfg.password = editPassword_ ? editPassword_->text().toStdString() : "";
  cfg.codec = comboCodec_ ? comboCodec_->currentData().toString().toStdString() : "h264";
  cfg.fps = sliderFps_ ? sliderFps_->value() : 60;

  QMetaObject::invokeMethod(worker_, "startClient", Qt::QueuedConnection,
                            Q_ARG(rc::SessionConfig, cfg));
}

void MainWindow::onStop() {
  if (worker_) worker_->stop();
}

void MainWindow::onLogMsg(const QString& tag, const QString& level, const QString& msg) {
  if (logView_) {
    QString colorTag = "#e0e0e0";
    if (level == "ERROR") colorTag = "#ff6b6b";
    else if (level == "WARN") colorTag = "#ffd166";
    logView_->append(QString("<span style='color:%1'>[%2]</span> %3")
                         .arg(colorTag).arg(tag, msg));
  }
}

void MainWindow::onStatusChanged(const QString& state) {
  if (statusText_) {
    if (state == "connected") statusText_->setText(tr("已连接"));
    else if (state == "registered") statusText_->setText(tr("在线 · 等待连接"));
    else if (state == "streaming") statusText_->setText(tr("传输中"));
    else if (state == "closed") statusText_->setText(tr("已断开"));
    else if (state == "auth_failed") statusText_->setText(tr("认证失败"));
    else statusText_->setText(state);
  }

  if (badgeConn_) {
    if (state == "connected" || state == "registered" || state == "streaming") {
      badgeConn_->setText("● " + statusText_->text());
      badgeConn_->setObjectName("BadgeOk");
      badgeConn_->style()->unpolish(badgeConn_);
      badgeConn_->style()->polish(badgeConn_);
    }
  }
}

void MainWindow::onProgressUpdated(int frames, qlonglong bytes) {
  Q_UNUSED(frames);
  if (badgeCapture_) {
    QString codecText = comboCodec_ ? comboCodec_->currentText() : "H.264";
    badgeCodec_->setText(codecText);
    badgeCodec_->setObjectName("BadgeOk");
    badgeCodec_->style()->unpolish(badgeCodec_);
    badgeCodec_->style()->polish(badgeCodec_);
  }
  if (logView_) {
    logView_->append(QString("progress: frames=%1 bytes=%2").arg(frames).arg(bytes));
  }
}

void MainWindow::onFrameReady(QImage image) {
  if (!videoView_ || image.isNull()) return;
  static_cast<VideoWidget*>(videoView_)->setFrame(image);
}

void MainWindow::onWorkerFinished(int rc) {
  Q_UNUSED(rc);
  if (statusText_) statusText_->setText(tr("已停止"));
}

void MainWindow::saveSettings() {
  QSettings s;
  s.setValue("relay/host", editRelayHost_ ? editRelayHost_->text() : "");
  s.setValue("relay/port", spinRelayPort_ ? spinRelayPort_->value() : 21116);
  s.setValue("device/id", editDeviceId_ ? editDeviceId_->text() : "");
  s.setValue("security/password", editPassword_ ? editPassword_->text() : "");
  s.setValue("security/force_pwd", switchForcePwd_ ? switchForcePwd_->isChecked() : false);
  s.setValue("security/aes", switchAes_ ? switchAes_->isChecked() : true);
  s.setValue("security/cert_verify", switchCertVerify_ ? switchCertVerify_->isChecked() : true);
  if (comboCodec_) s.setValue("display/codec", comboCodec_->currentData().toString());
  s.setValue("display/fps", sliderFps_ ? sliderFps_->value() : 60);
  s.setValue("display/quality", sliderQuality_ ? sliderQuality_->value() : 75);
  if (comboResolution_) s.setValue("display/resolution", comboResolution_->currentIndex());
  s.setValue("display/auto_bitrate", switchAutoBitrate_ ? switchAutoBitrate_->isChecked() : false);
  s.setValue("network/timeout", sliderTimeout_ ? sliderTimeout_->value() : 30);
  s.setValue("network/auto_reconnect", switchAutoReconnect_ ? switchAutoReconnect_->isChecked() : true);
  s.setValue("network/tcp_no_delay", switchTcpNoDelay_ ? switchTcpNoDelay_->isChecked() : true);
  s.setValue("perm/keyboard", switchKeyboard_ ? switchKeyboard_->isChecked() : true);
  s.setValue("perm/mouse", switchMouse_ ? switchMouse_->isChecked() : true);
  s.setValue("perm/clipboard", switchClipboard_ ? switchClipboard_->isChecked() : false);
  s.setValue("perm/file_transfer", switchFileTransfer_ ? switchFileTransfer_->isChecked() : false);
  s.setValue("perm/audio", switchAudio_ ? switchAudio_->isChecked() : false);
  s.setValue("perm/multi_monitor", switchMultiMonitor_ ? switchMultiMonitor_->isChecked() : true);
}

void MainWindow::loadSettings() {
  QSettings s;
  if (editRelayHost_) editRelayHost_->setText(s.value("relay/host", "127.0.0.1").toString());
  if (spinRelayPort_) spinRelayPort_->setValue(s.value("relay/port", 21116).toInt());
  if (editDeviceId_) editDeviceId_->setText(s.value("device/id", "").toString());
  if (editPassword_) editPassword_->setText(s.value("security/password", "").toString());
  if (switchForcePwd_) switchForcePwd_->setChecked(s.value("security/force_pwd", false).toBool());
  if (switchAes_) switchAes_->setChecked(s.value("security/aes", true).toBool());
  if (switchCertVerify_) switchCertVerify_->setChecked(s.value("security/cert_verify", true).toBool());
  if (comboCodec_) {
    const QString codec = s.value("display/codec", "h264").toString();
    const int idx = comboCodec_->findData(codec);
    if (idx >= 0) comboCodec_->setCurrentIndex(idx);
  }
  if (sliderFps_) sliderFps_->setValue(s.value("display/fps", 60).toInt());
  if (sliderQuality_) sliderQuality_->setValue(s.value("display/quality", 75).toInt());
  if (comboResolution_) comboResolution_->setCurrentIndex(s.value("display/resolution", 0).toInt());
  if (switchAutoBitrate_) switchAutoBitrate_->setChecked(s.value("display/auto_bitrate", false).toBool());
  if (sliderTimeout_) sliderTimeout_->setValue(s.value("network/timeout", 30).toInt());
  if (switchAutoReconnect_) switchAutoReconnect_->setChecked(s.value("network/auto_reconnect", true).toBool());
  if (switchTcpNoDelay_) switchTcpNoDelay_->setChecked(s.value("network/tcp_no_delay", true).toBool());
  if (switchKeyboard_) switchKeyboard_->setChecked(s.value("perm/keyboard", true).toBool());
  if (switchMouse_) switchMouse_->setChecked(s.value("perm/mouse", true).toBool());
  if (switchClipboard_) switchClipboard_->setChecked(s.value("perm/clipboard", false).toBool());
  if (switchFileTransfer_) switchFileTransfer_->setChecked(s.value("perm/file_transfer", false).toBool());
  if (switchAudio_) switchAudio_->setChecked(s.value("perm/audio", false).toBool());
  if (switchMultiMonitor_) switchMultiMonitor_->setChecked(s.value("perm/multi_monitor", true).toBool());
}

void MainWindow::initTrayIcon() {
  if (!QSystemTrayIcon::isSystemTrayAvailable()) {
    RC_LOGW("ui", "system tray not available, skip tray icon");
    return;
  }

  QIcon icon = style()->standardIcon(QStyle::SP_ComputerIcon);

  trayMenu_ = new QMenu(this);
  QAction* actShow = trayMenu_->addAction(tr("显示主窗口"));
  trayMenu_->addSeparator();
  QAction* actHost = trayMenu_->addAction(tr("启动被控端"));
  QAction* actClient = trayMenu_->addAction(tr("启动主控端"));
  QAction* actStop = trayMenu_->addAction(tr("停止"));
  trayMenu_->addSeparator();
  QAction* actQuit = trayMenu_->addAction(tr("退出"));

  trayIcon_ = new QSystemTrayIcon(icon, this);
  trayIcon_->setContextMenu(trayMenu_);
  trayIcon_->setToolTip(tr("远程控制 PC"));
  trayIcon_->show();

  connect(trayIcon_, &QSystemTrayIcon::activated,
          this, &MainWindow::onTrayActivated);
  connect(actShow, &QAction::triggered, this, [this]() {
    showNormal();
    activateWindow();
    raise();
  });
  connect(actHost, &QAction::triggered, this, &MainWindow::onStartHost);
  connect(actClient, &QAction::triggered, this, &MainWindow::onStartClient);
  connect(actStop, &QAction::triggered, this, &MainWindow::onStop);
  connect(actQuit, &QAction::triggered, this, [this]() {
    saveSettings();
    if (worker_) worker_->stop();
    qApp->quit();
  });
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
  if (reason == QSystemTrayIcon::DoubleClick ||
      reason == QSystemTrayIcon::Trigger) {
    if (isVisible()) {
      hide();
    } else {
      showNormal();
      activateWindow();
      raise();
    }
  }
}
