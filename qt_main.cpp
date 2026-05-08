#include "qt_recorder_window.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyleFactory>

static void applyDarkPalette(QApplication &app) {
  app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

  QPalette palette;
  palette.setColor(QPalette::Window, QColor(15, 17, 23));
  palette.setColor(QPalette::WindowText, QColor(231, 231, 234));
  palette.setColor(QPalette::Base, QColor(18, 21, 29));
  palette.setColor(QPalette::AlternateBase, QColor(26, 31, 42));
  palette.setColor(QPalette::ToolTipBase, QColor(231, 231, 234));
  palette.setColor(QPalette::ToolTipText, QColor(15, 17, 23));
  palette.setColor(QPalette::Text, QColor(231, 231, 234));
  palette.setColor(QPalette::Button, QColor(27, 34, 48));
  palette.setColor(QPalette::ButtonText, QColor(231, 231, 234));
  palette.setColor(QPalette::BrightText, QColor(255, 80, 80));
  palette.setColor(QPalette::Highlight, QColor(120, 199, 255));
  palette.setColor(QPalette::HighlightedText, QColor(15, 17, 23));
  app.setPalette(palette);
}

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  applyDarkPalette(app);

  RecorderWindow window;
  window.show();

  return app.exec();
}
