#include <QApplication>
#include <QFile>

#include "main_window.h"

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  app.setApplicationName("远程控制");
  app.setApplicationVersion("0.1.0");
  qRegisterMetaType<rc::SessionConfig>("rc::SessionConfig");

  // 样式表编译进二进制（resources.qrc），Windows/Linux 表现完全一致。
  QFile qss(":/style.qss");
  if (qss.open(QIODevice::ReadOnly)) {
    app.setStyleSheet(qss.readAll());
  }

  MainWindow w;
  w.resize(960, 680);
  w.show();
  return app.exec();
}
