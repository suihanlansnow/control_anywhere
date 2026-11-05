#include <QApplication>
#include <QFile>
#include <QTextStream>

#include "main_window.h"

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  app.setApplicationName("远程控制");
  app.setApplicationVersion("0.1.0");

  QFile qss(":/style.qss");
  if (qss.exists() && qss.open(QIODevice::ReadOnly)) {
    app.setStyleSheet(qss.readAll());
    qss.close();
  } else {
    QFile localQss("apps/qt/style.qss");
    if (localQss.open(QIODevice::ReadOnly)) {
      app.setStyleSheet(localQss.readAll());
      localQss.close();
    }
  }

  MainWindow w;
  w.resize(960, 680);
  w.show();
  return app.exec();
}
