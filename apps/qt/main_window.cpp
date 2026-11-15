#include <QApplication>
#include "main_window.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

// 同一套 UI 在两平台复用，仅采集后端与默认设备名按平台切换。
#if defined(_WIN32)
static constexpr const char* kPlatformPrefix = "win";
static constexpr const char* kPlatformFallbackHostId = "win-host";
static constexpr const char* kPlatformClientId = "win-client";
static constexpr const char* kPlatformCapture = "dxgi";
static constexpr const char* kPlatformName = "Windows";
#else
static constexpr const char* kPlatformPrefix = "linux";
static constexpr const char* kPlatformFallbackHostId = "linux-host";
static constexpr const char* kPlatformClientId = "linux-client";
static constexpr const char* kPlatformCapture = "x11";
static constexpr const char* kPlatformName = "Linux";
#endif

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QMenu>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSet>
#include <QSettings>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QSysInfo>
#include <QTextEdit>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

namespace {

// 只保留字母数字与短横线，避免设备 ID 里出现空格、点号等在命令行/URL 中需要转义的字符。
QString sanitizeId(const QString& text) {
  QString out;
  for (const QChar ch : text) {
    if (ch.isLetterOrNumber()) {
      out.append(ch.toLower());
    } else if (ch == '-' || ch == '_') {
      out.append(ch);
    }
  }
  return out.left(24);
}

// 首次启动时生成平台前缀 + 主机名的唯一设备 ID，避免多台机器默认 ID 撞号。
QString defaultDeviceId() {
  QString host = sanitizeId(QSysInfo::machineHostName());
  if (host.isEmpty()) return QString::fromLatin1(kPlatformFallbackHostId);
  if (!host.startsWith(QString::fromLatin1(kPlatformPrefix) + "-")) {
    host.prepend(QString::fromLatin1(kPlatformPrefix) + "-");
  }
  return host.left(32);
}

QString randomPassword() {
  static const char kChars[] = "abcdefghjkmnpqrstuvwxyz23456789";
  const int count = static_cast<int>(sizeof(kChars)) - 1;
  QString out;
  for (int i = 0; i < 8; ++i) {
    out.append(QChar(kChars[QRandomGenerator::global()->bounded(count)]));
  }
  return out;
}

class VideoWidget : public QWidget {
 public:
  using InputCallback = std::function<void(const rc::InputEvent&)>;

  explicit VideoWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setMinimumHeight(320);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
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

  void setInputCallback(InputCallback callback) { input_callback_ = std::move(callback); }

  void toggleFullscreen() {
    if (isFullScreen()) {
      releaseAll();
      showNormal();
      if (home_parent_) {
        setParent(home_parent_, Qt::Widget);
        if (home_layout_) home_layout_->insertWidget(home_index_, this, 1);
      }
      show();
    } else {
      home_parent_ = parentWidget();
      home_layout_ = home_parent_ ? qobject_cast<QVBoxLayout*>(home_parent_->layout()) : nullptr;
      home_index_ = home_layout_ ? home_layout_->indexOf(this) : 0;
      setParent(nullptr, Qt::Window | Qt::FramelessWindowHint);
      showFullScreen();
      activateWindow();
      setFocus(Qt::ActiveWindowFocusReason);
    }
    update();
  }

  void releaseAll() {
    if (!input_callback_) {
      pressed_keys_.clear();
      pressed_buttons_.clear();
      return;
    }
    const auto keys = pressed_keys_;
    const auto buttons = pressed_buttons_;
    pressed_keys_.clear();
    pressed_buttons_.clear();
    for (int key : keys) {
      rc::InputEvent event;
      event.type = rc::MsgType::InputKey;
      event.key = key;
      event.state = 0;
      input_callback_(event);
    }
    for (int button : buttons) {
      rc::InputEvent event;
      event.type = rc::MsgType::InputMouseButton;
      event.button = button;
      event.state = 0;
      event.x = last_remote_x_;
      event.y = last_remote_y_;
      input_callback_(event);
    }
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.fillRect(rect(), QColor("#101014"));

    if (frame_.isNull()) {
      p.setPen(QColor("#8a8f98"));
      p.drawText(rect(), Qt::AlignCenter, placeholder_);
      return;
    }
    p.drawImage(frameRect(), frame_);
    if (isFullScreen()) {
      p.fillRect(QRect(12, 12, 188, 30), QColor(0, 0, 0, 150));
      p.setPen(QColor("#e0e0e0"));
      p.drawText(QRect(22, 12, 178, 30), Qt::AlignVCenter, tr("F11 / Esc 退出全屏"));
    }
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    sendMouseMove(event->pos());
    event->accept();
  }

  void mousePressEvent(QMouseEvent* event) override {
    setFocus(Qt::MouseFocusReason);
    sendMouseButton(event, true);
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    sendMouseButton(event, false);
    event->accept();
  }

  void mouseDoubleClickEvent(QMouseEvent* event) override {
    sendMouseButton(event, true);
    event->accept();
  }

  void wheelEvent(QWheelEvent* event) override {
    const QPoint delta = event->angleDelta();
    if (!frame_.isNull() && input_callback_ && !delta.isNull()) {
      rc::InputEvent input;
      input.type = rc::MsgType::InputMouseWheel;
      input.delta_x = delta.x();
      input.delta_y = delta.y();
      input_callback_(input);
    }
    event->accept();
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (event->key() == Qt::Key_F11) {
      toggleFullscreen();
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_Escape && isFullScreen()) {
      toggleFullscreen();
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_unknown || !input_callback_ || frame_.isNull()) {
      QWidget::keyPressEvent(event);
      return;
    }
    const int key = event->key();
    if (!event->isAutoRepeat()) pressed_keys_.insert(key);
    rc::InputEvent input;
    input.type = rc::MsgType::InputKey;
    input.key = key;
    input.state = event->isAutoRepeat() ? 2 : 1;
    input_callback_(input);
    event->accept();
  }

  void keyReleaseEvent(QKeyEvent* event) override {
    if (event->key() == Qt::Key_F11) {
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_unknown || !input_callback_ || frame_.isNull()) {
      QWidget::keyReleaseEvent(event);
      return;
    }
    const int key = event->key();
    pressed_keys_.remove(key);
    rc::InputEvent input;
    input.type = rc::MsgType::InputKey;
    input.key = key;
    input.state = 0;
    input_callback_(input);
    event->accept();
  }

  void focusOutEvent(QFocusEvent* event) override {
    releaseAll();
    QWidget::focusOutEvent(event);
  }

 private:
  QRect frameRect() const {
    if (frame_.isNull()) return {};
    const QSize size = frame_.size().scaled(rect().size(), Qt::KeepAspectRatio);
    return QRect((width() - size.width()) / 2, (height() - size.height()) / 2,
                 size.width(), size.height());
  }

  bool remotePosition(const QPoint& point, int& x, int& y) const {
    const QRect target = frameRect();
    if (!target.isValid() || !target.contains(point)) return false;
    x = std::clamp((point.x() - target.x()) * frame_.width() / target.width(),
                   0, frame_.width() - 1);
    y = std::clamp((point.y() - target.y()) * frame_.height() / target.height(),
                   0, frame_.height() - 1);
    return true;
  }

  static int mouseButton(Qt::MouseButton button) {
    switch (button) {
      case Qt::LeftButton: return 1;
      case Qt::RightButton: return 2;
      case Qt::MiddleButton: return 3;
      case Qt::BackButton: return 4;
      case Qt::ForwardButton: return 5;
      default: return 0;
    }
  }

  void sendMouseMove(const QPoint& point) {
    int x = 0;
    int y = 0;
    if (!input_callback_ || !remotePosition(point, x, y)) return;
    last_remote_x_ = x;
    last_remote_y_ = y;
    rc::InputEvent input;
    input.type = rc::MsgType::InputMouseMove;
    input.x = x;
    input.y = y;
    input_callback_(input);
  }

  void sendMouseButton(QMouseEvent* mouse, bool pressed) {
    const int button = mouseButton(mouse->button());
    if (button == 0 || !input_callback_) return;
    int x = last_remote_x_;
    int y = last_remote_y_;
    if (!remotePosition(mouse->pos(), x, y)) {
      if (pressed) return;
      x = last_remote_x_;
      y = last_remote_y_;
    }
    sendMouseMove(mouse->pos());
    last_remote_x_ = x;
    last_remote_y_ = y;
    if (pressed) pressed_buttons_.insert(button);
    else pressed_buttons_.remove(button);
    rc::InputEvent input;
    input.type = rc::MsgType::InputMouseButton;
    input.button = button;
    input.state = pressed ? 1 : 0;
    input.x = x;
    input.y = y;
    input_callback_(input);
  }

  QImage frame_;
  QString placeholder_{tr("等待远程画面…")};
  InputCallback input_callback_;
  QSet<int> pressed_keys_;
  QSet<int> pressed_buttons_;
  int last_remote_x_ = 0;
  int last_remote_y_ = 0;
  QWidget* home_parent_ = nullptr;
  QVBoxLayout* home_layout_ = nullptr;
  int home_index_ = 0;
};

// ICE 服务器输入：分号或逗号分隔，忽略空项。
std::vector<std::string> split_ice_servers(const QString& text) {
  std::vector<std::string> servers;
  QString normalized = text;
  normalized.replace(',', ';');
  for (const QString& part : normalized.split(';', Qt::SkipEmptyParts)) {
    const std::string uri = part.trimmed().toStdString();
    if (!uri.empty()) servers.push_back(uri);
  }
  return servers;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(tr("远程控制"));
  setObjectName("MainWindow");
  buildUI();

  // 被控端与主控端必须各自独占线程：两个 Runner 的 run() 都是阻塞的，
  // 共用同一个 worker 会出现在"已开启被控"后无法再连接其他设备的问题。
  hostThread_ = new QThread(this);
  hostWorker_ = new BackendWorker();
  hostWorker_->moveToThread(hostThread_);
  connect(hostThread_, &QThread::finished, hostWorker_, &QObject::deleteLater);
  connect(hostWorker_, &BackendWorker::logMessage, this, &MainWindow::onLogMsg);
  connect(hostWorker_, &BackendWorker::statusChanged, this, &MainWindow::onHostStatus);
  connect(hostWorker_, &BackendWorker::finished, this, &MainWindow::onHostFinished);
  connect(hostWorker_, &BackendWorker::confirmationRequested, this,
          &MainWindow::onConfirmationRequested);
  hostThread_->start();

  clientThread_ = new QThread(this);
  clientWorker_ = new BackendWorker();
  clientWorker_->moveToThread(clientThread_);
  connect(clientThread_, &QThread::finished, clientWorker_, &QObject::deleteLater);
  connect(clientWorker_, &BackendWorker::logMessage, this, &MainWindow::onLogMsg);
  connect(clientWorker_, &BackendWorker::statusChanged, this, &MainWindow::onClientStatus);
  connect(clientWorker_, &BackendWorker::progressUpdated, this, &MainWindow::onProgressUpdated);
  connect(clientWorker_, &BackendWorker::frameReady, this, &MainWindow::onFrameReady);
  connect(clientWorker_, &BackendWorker::finished, this, &MainWindow::onClientFinished);
  clientThread_->start();

  loadSettings();
  ensureDeviceIdentity();
  initTrayIcon();

  // 程序启动即自动开启被控端，远端只需输入设备 ID 与密码即可接入。
  autoStartHost();
}

MainWindow::~MainWindow() {
  saveSettings();
  if (videoView_) {
    auto* video = static_cast<VideoWidget*>(videoView_);
    video->releaseAll();
    if (video->isFullScreen()) video->toggleFullscreen();
  }
  if (hostThread_) {
    if (hostWorker_) hostWorker_->stop();
    hostThread_->quit();
    hostThread_->wait(2000);
  }
  if (clientThread_) {
    if (clientWorker_) clientWorker_->stop();
    clientThread_->quit();
    clientThread_->wait(2000);
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
  panels_->addWidget(buildConnectPanel());
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
    {tr("连接"), 0}, {tr("显示"), 1}, {tr("网络"), 2}, {tr("权限"), 3}, {tr("关于"), 4}
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

QWidget* MainWindow::buildConnectPanel() {
  auto* panel = new QWidget();
  auto* scroll = new QVBoxLayout(panel);
  scroll->setContentsMargins(32, 24, 32, 24);
  scroll->setSpacing(16);

  auto* header = new QWidget();
  auto* hl = new QVBoxLayout(header);
  hl->setContentsMargins(0, 0, 0, 4);
  hl->setSpacing(4);
  auto* title = new QLabel(tr("连接"));
  title->setObjectName("PanelTitle");
  auto* desc = new QLabel(tr("本机启动后已自动开启被控端。远端只要输入下面的设备 ID 与密码即可连接；"
                             "本机也可以在这里连接其他设备。"));
  desc->setObjectName("PanelDesc");
  desc->setWordWrap(true);
  hl->addWidget(title);
  hl->addWidget(desc);
  scroll->addWidget(header);

  auto* card1 = makeCard(tr("本机（被控端 · 已自动开启）"));
  {
    auto* vbox = static_cast<QVBoxLayout*>(card1->layout());

    auto* idRow = new QHBoxLayout();
    idRow->setSpacing(10);
    auto* idCol = new QVBoxLayout();
    auto* lblId = new QLabel(tr("设备 ID（远端填写此项）"));
    lblId->setObjectName("FieldLabel");
    editDeviceId_ = new QLineEdit();
    editDeviceId_->setPlaceholderText(tr("自动生成"));
    idCol->addWidget(lblId);
    idCol->addWidget(editDeviceId_);
    idRow->addLayout(idCol, 1);
    auto* copyIdBtn = new QPushButton(tr("复制"));
    copyIdBtn->setObjectName("BtnGhost");
    idRow->addWidget(copyIdBtn, 0, Qt::AlignBottom);
    vbox->addLayout(idRow);

    vbox->addSpacing(4);
    auto* pwdRow = new QHBoxLayout();
    pwdRow->setSpacing(10);
    auto* pwdCol = new QVBoxLayout();
    auto* lblPwd = new QLabel(tr("连接密码"));
    lblPwd->setObjectName("FieldLabel");
    editPassword_ = new QLineEdit();
    editPassword_->setEchoMode(QLineEdit::Password);
    editPassword_->setPlaceholderText(tr("自动生成"));
    pwdCol->addWidget(lblPwd);
    pwdCol->addWidget(editPassword_);
    pwdRow->addLayout(pwdCol, 1);
    btnPasswordToggle_ = new QPushButton(tr("显示"));
    btnPasswordToggle_->setObjectName("BtnGhost");
    auto* copyPwdBtn = new QPushButton(tr("复制"));
    copyPwdBtn->setObjectName("BtnGhost");
    auto* regenBtn = new QPushButton(tr("重置"));
    regenBtn->setObjectName("BtnGhost");
    pwdRow->addWidget(btnPasswordToggle_, 0, Qt::AlignBottom);
    pwdRow->addWidget(copyPwdBtn, 0, Qt::AlignBottom);
    pwdRow->addWidget(regenBtn, 0, Qt::AlignBottom);
    vbox->addLayout(pwdRow);

    auto* hint = new QLabel(tr("首次启动会自动生成设备 ID 与密码并保存，可自行修改。"));
    hint->setObjectName("FieldHint");
    vbox->addWidget(hint);

    vbox->addSpacing(8);
    auto* inputRow = new QVBoxLayout();
    inputRow->setSpacing(2);
    switchRemoteInput_ = new QCheckBox(tr("允许远程键鼠操作"));
    switchRemoteInput_->setObjectName("Switch");
    switchRemoteInput_->setChecked(true);
    auto* inputHint = new QLabel(tr("取消勾选后进入仅观看模式：远端仍能看到画面，但鼠标和键盘不会生效"));
    inputHint->setObjectName("FieldHint");
    inputHint->setWordWrap(true);
    inputRow->addWidget(switchRemoteInput_);
    inputRow->addWidget(inputHint);
    vbox->addLayout(inputRow);

    vbox->addSpacing(8);
    auto* confirmRow = new QVBoxLayout();
    confirmRow->setSpacing(2);
    switchConfirm_ = new QCheckBox(tr("连接前需要本机确认"));
    switchConfirm_->setObjectName("Switch");
    switchConfirm_->setChecked(true);
    auto* confirmHint =
        new QLabel(tr("有主控端请求连接时弹窗询问；无人值守运行请取消勾选"));
    confirmHint->setObjectName("FieldHint");
    confirmHint->setWordWrap(true);
    confirmRow->addWidget(switchConfirm_);
    confirmRow->addWidget(confirmHint);
    vbox->addLayout(confirmRow);

    vbox->addSpacing(8);
    auto* statusRow = new QHBoxLayout();
    statusRow->setSpacing(10);
    hostStatusLabel_ = new QLabel(tr("正在开启…"));
    hostStatusLabel_->setObjectName("FieldHint");
    btnHostStart_ = new QPushButton(tr("重新开启"));
    btnHostStart_->setObjectName("BtnGhost");
    btnHostStop_ = new QPushButton(tr("停止被控"));
    btnHostStop_->setObjectName("BtnGhost");
    statusRow->addWidget(hostStatusLabel_, 1);
    statusRow->addWidget(btnHostStart_);
    statusRow->addWidget(btnHostStop_);
    vbox->addLayout(statusRow);

    connect(copyIdBtn, &QPushButton::clicked, this, &MainWindow::onCopyDeviceId);
    connect(copyPwdBtn, &QPushButton::clicked, this, &MainWindow::onCopyHostPassword);
    connect(btnPasswordToggle_, &QPushButton::clicked, this,
            &MainWindow::onToggleHostPassword);
    connect(regenBtn, &QPushButton::clicked, this,
            &MainWindow::onRegenerateHostPassword);
    connect(switchRemoteInput_, &QCheckBox::toggled, this, [this](bool enabled) {
      // 被控线程正阻塞在 run() 中，排队调用不会被执行，这里直接调用：
      // setInputEnabled 只写原子变量并在锁内调用 runner，不会阻塞 UI。
      if (hostWorker_) hostWorker_->setInputEnabled(enabled);
      if (hostStatusLabel_ && hostState_ == "streaming") {
        hostStatusLabel_->setText(enabled ? tr("正在被远端控制")
                                          : tr("远端正在观看 · 键鼠已禁用"));
      }
      if (logView_) {
        logView_->append(enabled ? tr("已允许远程键鼠操作")
                                 : tr("已切换为仅观看模式（远程键鼠已禁用）"));
      }
    });
  }
  scroll->addWidget(card1);

  auto* card2 = makeCard(tr("连接远程设备"));
  {
    auto* vbox = static_cast<QVBoxLayout*>(card2->layout());

    auto* relayRow = new QHBoxLayout();
    relayRow->setSpacing(14);
    auto* hostCol = new QVBoxLayout();
    auto* lblHost = new QLabel(tr("中继服务器地址"));
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
    spinRelayPort_->setMaximumWidth(110);
    portCol->addWidget(lblPort);
    portCol->addWidget(spinRelayPort_);
    relayRow->addLayout(hostCol, 1);
    relayRow->addLayout(portCol);
    vbox->addLayout(relayRow);

    auto* tlsRow = new QHBoxLayout();
    tlsRow->setSpacing(10);
    switchTls_ = new QCheckBox(tr("TLS 加密"));
    editTlsFingerprint_ = new QLineEdit();
    editTlsFingerprint_->setPlaceholderText(
        tr("中继证书 SHA-256 指纹（留空则仅加密、不校验证书）"));
    tlsRow->addWidget(switchTls_);
    tlsRow->addWidget(editTlsFingerprint_, 1);
    vbox->addLayout(tlsRow);

    vbox->addSpacing(4);
    auto* lblTarget = new QLabel(tr("被控端设备 ID"));
    lblTarget->setObjectName("FieldLabel");
    editTargetId_ = new QLineEdit();
    editTargetId_->setPlaceholderText(tr("对方程序首页显示的设备 ID"));
    vbox->addWidget(lblTarget);
    vbox->addWidget(editTargetId_);

    vbox->addSpacing(4);
    auto* lblConnectPwd = new QLabel(tr("被控端密码"));
    lblConnectPwd->setObjectName("FieldLabel");
    editConnectPassword_ = new QLineEdit();
    editConnectPassword_->setEchoMode(QLineEdit::Password);
    editConnectPassword_->setPlaceholderText(tr("对方程序首页显示的连接密码"));
    vbox->addWidget(lblConnectPwd);
    vbox->addWidget(editConnectPassword_);

    vbox->addSpacing(8);
    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);
    btnConnect_ = new QPushButton(tr("连接远程桌面"));
    btnConnect_->setObjectName("BtnPrimary");
    btnDisconnect_ = new QPushButton(tr("断开"));
    btnDisconnect_->setObjectName("BtnGhost");
    btnSendFile_ = new QPushButton(tr("发送文件…"));
    btnSendFile_->setObjectName("BtnGhost");
    clientStatusLabel_ = new QLabel(tr("未连接"));
    clientStatusLabel_->setObjectName("FieldHint");
    clientStatusLabel_->setWordWrap(true);
    btnRow->addWidget(btnConnect_);
    btnRow->addWidget(btnSendFile_);
    btnRow->addWidget(btnDisconnect_);
    btnRow->addStretch();
    vbox->addLayout(btnRow);
    vbox->addWidget(clientStatusLabel_);
  }
  scroll->addWidget(card2);
  scroll->addStretch();

  connect(btnHostStart_, &QPushButton::clicked, this, &MainWindow::onStartHost);
  connect(btnHostStop_, &QPushButton::clicked, this, &MainWindow::onStopHost);
  connect(btnConnect_, &QPushButton::clicked, this, &MainWindow::onStartClient);
  connect(btnDisconnect_, &QPushButton::clicked, this, &MainWindow::onStopClient);
  connect(btnSendFile_, &QPushButton::clicked, this, &MainWindow::onSendFile);
  connect(editConnectPassword_, &QLineEdit::returnPressed, this,
          &MainWindow::onStartClient);
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
  auto* desc = new QLabel(tr("远程画面按实际视频帧显示，支持全屏及远程鼠标、键盘操作。"));
  desc->setObjectName("PanelDesc");
  desc->setWordWrap(true);
  hl->addWidget(title);
  hl->addWidget(desc);
  scroll->addWidget(header);

  auto* cardVideo = makeCard(tr("远程画面"));
  cardVideo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  {
    auto* vbox = static_cast<QVBoxLayout*>(cardVideo->layout());
    auto* toolRow = new QHBoxLayout();
    auto* inputHint = new QLabel(tr("单击画面后即可操作远程系统，F11 全屏，Esc 退出"));
    inputHint->setObjectName("FieldHint");
    auto* fullscreenBtn = new QPushButton(tr("全屏（F11）"));
    fullscreenBtn->setObjectName("BtnGhost");
    toolRow->addWidget(inputHint, 1);
    toolRow->addWidget(fullscreenBtn);
    vbox->addLayout(toolRow);

    videoView_ = new VideoWidget();
    vbox->addWidget(videoView_, 1);
    auto* video = static_cast<VideoWidget*>(videoView_);
    connect(fullscreenBtn, &QPushButton::clicked, video, &VideoWidget::toggleFullscreen);
    video->setInputCallback([this](const rc::InputEvent& event) {
      if (!clientWorker_) return;
      if (event.type == rc::MsgType::InputKey) {
        if (switchKeyboard_ && !switchKeyboard_->isChecked()) return;
      } else if (switchMouse_ && !switchMouse_->isChecked()) {
        return;
      }
      clientWorker_->sendInput(event);
    });
  }
  scroll->addWidget(cardVideo, 1);

  auto* card2 = makeCard(tr("编码"));
  {
    auto* vbox = static_cast<QVBoxLayout*>(card2->layout());
    // 编码固定为 H.264：服务端只提供 H.264 编码器，客户端也只内置 H.264 解码器。
    auto* lblCodec = new QLabel(tr("视频编码"));
    lblCodec->setObjectName("FieldLabel");
    auto* codecValue = new QLabel(tr("H.264（硬件加速，自动回退软件编码）"));
    vbox->addWidget(lblCodec);
    vbox->addWidget(codecValue);

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
  btnRow->addWidget(saveBtn);
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
  auto* desc = new QLabel(tr("中继地址与被控端信息已在“连接”页配置，这里保留高级连接参数与运行日志。"));
  desc->setObjectName("PanelDesc");
  desc->setWordWrap(true);
  hl->addWidget(title);
  hl->addWidget(desc);
  scroll->addWidget(header);

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

#ifdef RC_ENABLE_WEBRTC
    auto* line3 = new QFrame();
    line3->setFrameShape(QFrame::HLine);
    vbox->addWidget(line3);

    switchWebRtc_ = new QCheckBox(tr("WebRTC 传输（实验）"));
    switchWebRtc_->setObjectName("Switch");
    switchWebRtc_->setChecked(false);
    auto* h3 = new QLabel(tr("视频走 RTP、输入走 DataChannel，信令仍经中继转发"));
    h3->setObjectName("FieldHint");
    auto* r3 = new QVBoxLayout();
    r3->setSpacing(2);
    r3->addWidget(switchWebRtc_);
    r3->addWidget(h3);
    vbox->addLayout(r3);

    editIceServers_ = new QLineEdit();
    editIceServers_->setPlaceholderText(
        tr("stun:stun.example.com:3478;turn:user:pass@turn.example.com:3478"));
    auto* h4 = new QLabel(tr("ICE 服务器，留空则使用中继下发的配置，多个用分号分隔"));
    h4->setObjectName("FieldHint");
    vbox->addWidget(editIceServers_);
    vbox->addWidget(h4);

    switchIceRelayOnly_ = new QCheckBox(tr("仅使用 TURN 中继"));
    switchIceRelayOnly_->setObjectName("Switch");
    switchIceRelayOnly_->setChecked(false);
    auto* h5 = new QLabel(tr("隐藏本机候选地址，强制经 TURN 转发（需配置可用 TURN）"));
    h5->setObjectName("FieldHint");
    auto* r4 = new QVBoxLayout();
    r4->setSpacing(2);
    r4->addWidget(switchIceRelayOnly_);
    r4->addWidget(h5);
    vbox->addLayout(r4);
#endif

    // 自动重连可在会话进行中即时生效，无需重启会话。
    connect(switchAutoReconnect_, &QCheckBox::toggled, this, [this](bool on) {
      if (clientWorker_) clientWorker_->setAutoReconnect(on);
    });
  }
  scroll->addWidget(card2);

  auto* btnRow = new QHBoxLayout();
  btnRow->setSpacing(10);
  auto* saveBtn = new QPushButton(tr("保存设置"));
  saveBtn->setObjectName("BtnPrimary");
  btnRow->addWidget(saveBtn);
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

  connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveSettings);

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

  connect(resetBtn, &QPushButton::clicked, this, [this] {
    // 恢复默认权限：键鼠、剪贴板、多显示器开启，文件与音频关闭。
    const struct { QCheckBox* box; bool value; } defaults[] = {
        {switchKeyboard_, true},  {switchMouse_, true},
        {switchClipboard_, true}, {switchFileTransfer_, false},
        {switchAudio_, false},    {switchMultiMonitor_, true},
    };
    for (const auto& item : defaults) {
      if (item.box) item.box->setChecked(item.value);
    }
    saveSettings();
    if (logView_) logView_->append(tr("权限已恢复默认"));
  });
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
    vbox->addWidget(makeInfoRow(tr("编码器"), mkVal("H.264")));
    vbox->addWidget(makeInfoRow(tr("平台"), mkVal(kPlatformName)));
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
    badgeCodec_ = mkBadge(tr("—"), "BadgeOff");

    vbox->addWidget(makeInfoRow(tr("连接状态"), badgeConn_));
    vbox->addWidget(makeInfoRow(tr("帧率 / 带宽"), badgeCodec_));
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

  connect(checkBtn, &QPushButton::clicked, this, [] {
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("https://github.com/suihanlansnow/control_anywhere/releases")));
  });
  connect(disconnectBtn, &QPushButton::clicked, this, &MainWindow::onStopClient);
  return panel;
}

void MainWindow::onSaveSettings() {
  saveSettings();
  if (logView_) logView_->append(tr("设置已保存"));
}

void MainWindow::ensureDeviceIdentity() {
  if (editDeviceId_ && editDeviceId_->text().trimmed().isEmpty()) {
    editDeviceId_->setText(defaultDeviceId());
  }
  if (editPassword_ && editPassword_->text().isEmpty()) {
    editPassword_->setText(randomPassword());
  }
  saveSettings();
}

void MainWindow::autoStartHost() { onStartHost(); }

void MainWindow::onStartHost() {
  if (!hostWorker_) return;
  // 先停掉已有会话再重启，避免同一个设备 ID 重复注册。
  hostWorker_->stop();
  saveSettings();

  rc::SessionConfig cfg;
  cfg.relay_host = editRelayHost_ && !editRelayHost_->text().trimmed().isEmpty()
                       ? editRelayHost_->text().trimmed().toStdString()
                       : "127.0.0.1";
  cfg.relay_port = spinRelayPort_ ? static_cast<uint16_t>(spinRelayPort_->value()) : 21116;
  cfg.device_id = editDeviceId_ && !editDeviceId_->text().trimmed().isEmpty()
                      ? editDeviceId_->text().trimmed().toStdString()
                      : kPlatformFallbackHostId;
  cfg.alias = kPlatformName;
  cfg.password = editPassword_ ? editPassword_->text().toStdString() : "";
  cfg.codec = "h264";
  cfg.capture = kPlatformCapture;
  cfg.fps = 30;
  cfg.max_frames = 0;
  cfg.connect_timeout_ms = sliderTimeout_ ? sliderTimeout_->value() * 1000 : 5000;
  cfg.tcp_no_delay = switchTcpNoDelay_ ? switchTcpNoDelay_->isChecked() : true;
  cfg.transport =
      (switchWebRtc_ && switchWebRtc_->isChecked()) ? "webrtc" : "tcp";
  cfg.ice_servers =
      editIceServers_ ? split_ice_servers(editIceServers_->text())
                      : std::vector<std::string>();
  cfg.ice_force_relay = switchIceRelayOnly_ && switchIceRelayOnly_->isChecked();
  cfg.clipboard = switchClipboard_ && switchClipboard_->isChecked();
  cfg.file_transfer = switchFileTransfer_ && switchFileTransfer_->isChecked();
  cfg.tls = switchTls_ && switchTls_->isChecked();
  cfg.tls_fingerprint = editTlsFingerprint_ ? editTlsFingerprint_->text().trimmed().toStdString()
                                            : std::string();
  cfg.tls_insecure = cfg.tls && cfg.tls_fingerprint.empty();
  cfg.require_confirmation = switchConfirm_ ? switchConfirm_->isChecked() : true;
  cfg.confirm_timeout_ms = 30000;

  hostState_ = "connecting";
  if (hostStatusLabel_) hostStatusLabel_->setText(tr("正在开启被控端…"));
  updateStatusLabel();

  QMetaObject::invokeMethod(hostWorker_, "startHost", Qt::QueuedConnection,
                            Q_ARG(rc::SessionConfig, cfg));
}

void MainWindow::onStopHost() {
  if (!hostWorker_) return;
  hostWorker_->stop();
  hostState_ = "closed";
  if (hostStatusLabel_) hostStatusLabel_->setText(tr("被控端已停止"));
  updateStatusLabel();
}

void MainWindow::onStartClient() {
  if (!clientWorker_) return;
  const QString target = editTargetId_ ? editTargetId_->text().trimmed() : QString();
  const QString password = editConnectPassword_ ? editConnectPassword_->text() : QString();
  if (target.isEmpty()) {
    if (clientStatusLabel_) clientStatusLabel_->setText(tr("请先输入被控端设备 ID"));
    if (editTargetId_) editTargetId_->setFocus();
    return;
  }
  if (password.isEmpty()) {
    if (clientStatusLabel_) clientStatusLabel_->setText(tr("请先输入被控端密码"));
    if (editConnectPassword_) editConnectPassword_->setFocus();
    return;
  }

  clientWorker_->stop();
  saveSettings();

  rc::SessionConfig cfg;
  cfg.relay_host = editRelayHost_ && !editRelayHost_->text().trimmed().isEmpty()
                       ? editRelayHost_->text().trimmed().toStdString()
                       : "127.0.0.1";
  cfg.relay_port = spinRelayPort_ ? static_cast<uint16_t>(spinRelayPort_->value()) : 21116;
  cfg.device_id = kPlatformClientId;
  cfg.target_id = target.toStdString();
  cfg.password = password.toStdString();
  cfg.codec = "h264";
  cfg.render = false;
  cfg.connect_timeout_ms = sliderTimeout_ ? sliderTimeout_->value() * 1000 : 5000;
  cfg.tcp_no_delay = switchTcpNoDelay_ ? switchTcpNoDelay_->isChecked() : true;
  cfg.transport =
      (switchWebRtc_ && switchWebRtc_->isChecked()) ? "webrtc" : "tcp";
  cfg.ice_servers =
      editIceServers_ ? split_ice_servers(editIceServers_->text())
                      : std::vector<std::string>();
  cfg.ice_force_relay = switchIceRelayOnly_ && switchIceRelayOnly_->isChecked();
  cfg.clipboard = switchClipboard_ && switchClipboard_->isChecked();
  cfg.file_transfer = switchFileTransfer_ && switchFileTransfer_->isChecked();
  cfg.tls = switchTls_ && switchTls_->isChecked();
  cfg.tls_fingerprint = editTlsFingerprint_ ? editTlsFingerprint_->text().trimmed().toStdString()
                                            : std::string();
  cfg.tls_insecure = cfg.tls && cfg.tls_fingerprint.empty();

  if (clientWorker_ && switchAutoReconnect_) {
    clientWorker_->setAutoReconnect(switchAutoReconnect_->isChecked());
  }

  clientState_ = "connecting";
  // 新会话的帧计数从头开始，避免与上一会话的累计值相减。
  lastProgressFrames_ = -1;
  lastProgressBytes_ = 0;
  if (clientStatusLabel_) clientStatusLabel_->setText(tr("正在连接 %1 …").arg(target));
  updateStatusLabel();

  QMetaObject::invokeMethod(clientWorker_, "startClient", Qt::QueuedConnection,
                            Q_ARG(rc::SessionConfig, cfg));
}

void MainWindow::onStopClient() {
  if (videoView_) static_cast<VideoWidget*>(videoView_)->releaseAll();
  if (clientWorker_) clientWorker_->stop();
  clientState_ = "closed";
  if (clientStatusLabel_) clientStatusLabel_->setText(tr("已断开远程连接"));
  updateStatusLabel();
}

void MainWindow::onSendFile() {
  if (!clientWorker_ || clientState_ != "streaming") {
    if (clientStatusLabel_) clientStatusLabel_->setText(tr("请先连接远程设备再发送文件"));
    return;
  }
  const QString path = QFileDialog::getOpenFileName(this, tr("选择要发送的文件"));
  if (path.isEmpty()) return;
  clientWorker_->sendFile(path);
  if (logView_) logView_->append(tr("正在发送文件：%1").arg(QFileInfo(path).fileName()));
}

void MainWindow::onCopyDeviceId() {
  if (!editDeviceId_) return;
  QApplication::clipboard()->setText(editDeviceId_->text().trimmed());
  QToolTip::showText(QCursor::pos(), tr("设备 ID 已复制"));
}

void MainWindow::onCopyHostPassword() {
  if (!editPassword_) return;
  QApplication::clipboard()->setText(editPassword_->text());
  QToolTip::showText(QCursor::pos(), tr("连接密码已复制"));
}

void MainWindow::onToggleHostPassword() {
  if (!editPassword_ || !btnPasswordToggle_) return;
  const bool hidden = editPassword_->echoMode() == QLineEdit::Password;
  editPassword_->setEchoMode(hidden ? QLineEdit::Normal : QLineEdit::Password);
  btnPasswordToggle_->setText(hidden ? tr("隐藏") : tr("显示"));
}

void MainWindow::onRegenerateHostPassword() {
  if (!editPassword_) return;
  editPassword_->setText(randomPassword());
  onStartHost();
}

void MainWindow::updateStatusLabel() {
  if (!statusText_) return;
  const bool clientActive = !clientState_.isEmpty() && clientState_ != "closed" &&
                            clientState_ != "idle";
  if (clientActive) {
    if (clientState_ == "streaming") statusText_->setText(tr("远程画面传输中"));
    else if (clientState_ == "connected") statusText_->setText(tr("已连接远程"));
    else if (clientState_ == "auth_failed") statusText_->setText(tr("远程密码错误"));
    else if (clientState_ == "retrying") statusText_->setText(tr("等待远程端…"));
    else statusText_->setText(tr("连接远程中…"));
    return;
  }
  if (hostState_ == "registered") statusText_->setText(tr("本机在线 · 等待连接"));
  else if (hostState_ == "connected" || hostState_ == "streaming") {
    if (switchRemoteInput_ && !switchRemoteInput_->isChecked()) {
      statusText_->setText(tr("他人正在观看本机（仅观看）"));
    } else {
      statusText_->setText(tr("他人正在控制本机"));
    }
  } else if (hostState_ == "auth_failed") {
    statusText_->setText(tr("鉴权失败"));
  } else if (hostState_ == "closed") {
    statusText_->setText(tr("被控已停止"));
  } else if (hostState_ == "connecting") {
    statusText_->setText(tr("正在连接中继…"));
  } else if (hostState_ == "retrying") {
    statusText_->setText(tr("被控重试中…"));
  } else {
    statusText_->setText(tr("未连接"));
  }
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

void MainWindow::onHostStatus(const QString& state) {
  hostState_ = state;
  if (hostStatusLabel_) {
    if (state == "connecting") hostStatusLabel_->setText(tr("正在连接中继…"));
    else if (state == "retrying") hostStatusLabel_->setText(tr("中继未连接 · 自动重试中"));
    else if (state == "connected") hostStatusLabel_->setText(tr("已连接中继，等待远端接入"));
    else if (state == "registered") hostStatusLabel_->setText(tr("在线 · 等待远端连接"));
    else if (state == "streaming") {
      hostStatusLabel_->setText(switchRemoteInput_ && !switchRemoteInput_->isChecked()
                                    ? tr("远端正在观看 · 键鼠已禁用")
                                    : tr("正在被远端控制"));
    }
    else if (state == "auth_failed") hostStatusLabel_->setText(tr("密码校验失败，请重试"));
    else if (state == "closed") hostStatusLabel_->setText(tr("被控端已停止"));
    else hostStatusLabel_->setText(state);
  }
  updateStatusLabel();
}

void MainWindow::onClientStatus(const QString& state) {
  clientState_ = state;
  if (clientStatusLabel_) {
    if (state == "connecting") clientStatusLabel_->setText(tr("正在连接中继…"));
    else if (state == "connected") clientStatusLabel_->setText(tr("已连接中继，等待画面…"));
    else if (state == "authing") clientStatusLabel_->setText(tr("正在鉴权…"));
    else if (state == "streaming") clientStatusLabel_->setText(tr("远程画面传输中"));
    else if (state == "auth_failed") clientStatusLabel_->setText(tr("被控端密码错误"));
    else if (state == "retrying") clientStatusLabel_->setText(tr("目标离线，等待被控端上线…"));
    else if (state == "closed") clientStatusLabel_->setText(tr("远程会话已断开"));
    else clientStatusLabel_->setText(state);
  }
  if (state == "streaming" || state == "connected") {
    if (panels_) panels_->setCurrentIndex(1);
  }
  if (badgeConn_) {
    if (state == "connected" || state == "streaming") {
      badgeConn_->setText("● " + clientStatusLabel_->text());
      badgeConn_->setObjectName("BadgeOk");
      badgeConn_->style()->unpolish(badgeConn_);
      badgeConn_->style()->polish(badgeConn_);
    }
  }
  updateStatusLabel();
}

void MainWindow::onProgressUpdated(int frames, qlonglong bytes) {
  // progress 回调是累计值，按两次回调的差值换算实时帧率与带宽。
  const auto now = std::chrono::steady_clock::now();
  if (lastProgressFrames_ >= 0 && frames > lastProgressFrames_) {
    const double seconds =
        std::chrono::duration<double>(now - lastProgressTime_).count();
    if (seconds >= 0.5) {
      const double fps = (frames - lastProgressFrames_) / seconds;
      const double mbps = static_cast<double>(bytes - lastProgressBytes_) * 8.0 /
                          seconds / 1000000.0;
      if (badgeCodec_) {
        badgeCodec_->setText(QString("%1 fps · %2 Mbps")
                                 .arg(fps, 0, 'f', 1)
                                 .arg(mbps, 0, 'f', 2));
        badgeCodec_->setObjectName("BadgeOk");
        badgeCodec_->style()->unpolish(badgeCodec_);
        badgeCodec_->style()->polish(badgeCodec_);
      }
      lastProgressFrames_ = frames;
      lastProgressBytes_ = bytes;
      lastProgressTime_ = now;
    }
  } else {
    lastProgressFrames_ = frames;
    lastProgressBytes_ = bytes;
    lastProgressTime_ = now;
  }
}

void MainWindow::onFrameReady(QImage image) {
  if (!videoView_ || image.isNull()) return;
  static_cast<VideoWidget*>(videoView_)->setFrame(image);
}

void MainWindow::onConfirmationRequested(qulonglong sessionId, const QString& peerId,
                                         const QString& peerAlias) {
  Q_UNUSED(sessionId);
  const QString who =
      peerAlias.isEmpty() ? peerId : tr("%1（%2）").arg(peerAlias, peerId);
  // 工作线程正阻塞等待结果，这里必须弹窗并回答，否则超时后按拒绝处理。
  const auto answer =
      QMessageBox::question(this, tr("远程连接请求"),
                            tr("%1 请求连接并控制本机，是否允许？").arg(who),
                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  const bool allow = answer == QMessageBox::Yes;
  if (hostWorker_) hostWorker_->answerConfirmation(allow);
  RC_LOGI("ui", std::string("remote control request from ") + peerId.toStdString() +
                    (allow ? " allowed" : " denied"));
}

void MainWindow::onHostFinished(int rc) {
  Q_UNUSED(rc);
  hostState_ = "closed";
  if (hostStatusLabel_) hostStatusLabel_->setText(tr("被控端已停止"));
  updateStatusLabel();
}

void MainWindow::onClientFinished(int rc) {
  Q_UNUSED(rc);
  clientState_ = "closed";
  if (clientStatusLabel_) clientStatusLabel_->setText(tr("远程会话已结束"));
  updateStatusLabel();
}

void MainWindow::saveSettings() {
  QSettings s;
  s.setValue("relay/host", editRelayHost_ ? editRelayHost_->text() : "");
  s.setValue("relay/port", spinRelayPort_ ? spinRelayPort_->value() : 21116);
  s.setValue("relay/tls", switchTls_ ? switchTls_->isChecked() : false);
  s.setValue("relay/tls_fingerprint",
             editTlsFingerprint_ ? editTlsFingerprint_->text() : "");
  s.setValue("device/id", editDeviceId_ ? editDeviceId_->text() : "");
  s.setValue("security/password", editPassword_ ? editPassword_->text() : "");
  s.setValue("security/confirm", switchConfirm_ ? switchConfirm_->isChecked() : true);
  s.setValue("client/target", editTargetId_ ? editTargetId_->text() : "");
  s.setValue("client/password", editConnectPassword_ ? editConnectPassword_->text() : "");
  s.setValue("display/auto_bitrate", switchAutoBitrate_ ? switchAutoBitrate_->isChecked() : false);
  s.setValue("network/timeout", sliderTimeout_ ? sliderTimeout_->value() : 30);
  s.setValue("network/auto_reconnect", switchAutoReconnect_ ? switchAutoReconnect_->isChecked() : true);
  s.setValue("network/tcp_no_delay", switchTcpNoDelay_ ? switchTcpNoDelay_->isChecked() : true);
  s.setValue("network/webrtc", switchWebRtc_ ? switchWebRtc_->isChecked() : false);
  s.setValue("network/ice_servers", editIceServers_ ? editIceServers_->text() : "");
  s.setValue("network/ice_relay_only",
             switchIceRelayOnly_ ? switchIceRelayOnly_->isChecked() : false);
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
  if (switchTls_) switchTls_->setChecked(s.value("relay/tls", false).toBool());
  if (editTlsFingerprint_) {
    editTlsFingerprint_->setText(s.value("relay/tls_fingerprint", "").toString());
  }
  if (spinRelayPort_) spinRelayPort_->setValue(s.value("relay/port", 21116).toInt());
  if (editDeviceId_) editDeviceId_->setText(s.value("device/id", "").toString());
  if (editPassword_) editPassword_->setText(s.value("security/password", "").toString());
  if (switchConfirm_) switchConfirm_->setChecked(s.value("security/confirm", true).toBool());
  if (editTargetId_) editTargetId_->setText(s.value("client/target", "").toString());
  if (editConnectPassword_) {
    editConnectPassword_->setText(s.value("client/password", "").toString());
  }
  if (switchAutoBitrate_) switchAutoBitrate_->setChecked(s.value("display/auto_bitrate", false).toBool());
  if (sliderTimeout_) sliderTimeout_->setValue(s.value("network/timeout", 30).toInt());
  if (switchAutoReconnect_) switchAutoReconnect_->setChecked(s.value("network/auto_reconnect", true).toBool());
  if (switchTcpNoDelay_) switchTcpNoDelay_->setChecked(s.value("network/tcp_no_delay", true).toBool());
  if (switchWebRtc_) switchWebRtc_->setChecked(s.value("network/webrtc", false).toBool());
  if (editIceServers_) editIceServers_->setText(s.value("network/ice_servers", "").toString());
  if (switchIceRelayOnly_) {
    switchIceRelayOnly_->setChecked(s.value("network/ice_relay_only", false).toBool());
  }
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
  QAction* actHost = trayMenu_->addAction(tr("重新开启被控端"));
  QAction* actClient = trayMenu_->addAction(tr("连接远程设备"));
  QAction* actStop = trayMenu_->addAction(tr("断开远程"));
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
  connect(actStop, &QAction::triggered, this, &MainWindow::onStopClient);
  connect(actQuit, &QAction::triggered, this, [this]() {
    saveSettings();
    if (hostWorker_) hostWorker_->stop();
    if (clientWorker_) clientWorker_->stop();
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
