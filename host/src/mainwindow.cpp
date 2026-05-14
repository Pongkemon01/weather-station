#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QIcon>

#include "app_info.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui_(new Ui::MainWindow) {
    ui_->setupUi(this);

    setWindowTitle(tr("%1 v%2")
                       .arg(QLatin1String(AppInfo::kAppDisplayName))
                       .arg(QLatin1String(AppInfo::kAppVersion)));
    setWindowIcon(QIcon(QStringLiteral(":/icons/robin_wsc.png")));
}

MainWindow::~MainWindow() {
    delete ui_;
}

void MainWindow::showBanner(const QString& message, BannerKind kind) {
    QString style;
    switch (kind) {
    case BannerKind::Info:
        style = QStringLiteral(
            "background-color: #d0e8ff; color: #003060; padding: 4px 8px;");
        break;
    case BannerKind::Warning:
        style = QStringLiteral(
            "background-color: #fff3cd; color: #664d03; padding: 4px 8px;");
        break;
    case BannerKind::Error:
        style = QStringLiteral(
            "background-color: #f8d7da; color: #58151c; padding: 4px 8px;");
        break;
    }
    ui_->bannerLabel->setStyleSheet(style);
    ui_->bannerLabel->setText(message);
    ui_->bannerLabel->setVisible(true);
}

void MainWindow::hideBanner() {
    ui_->bannerLabel->setVisible(false);
    ui_->bannerLabel->clear();
}
