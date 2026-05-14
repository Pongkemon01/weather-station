// main.cpp — application entry point.

#include <QApplication>
#include <QIcon>

#include "app_info.h"
#include "mainwindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QApplication::setApplicationName(AppInfo::kAppDisplayName);
    QApplication::setApplicationDisplayName(AppInfo::kAppDisplayName);
    QApplication::setApplicationVersion(AppInfo::kAppVersion);
    QApplication::setOrganizationName(AppInfo::kOrganizationName);
    QApplication::setOrganizationDomain(AppInfo::kOrganizationDomain);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/robin_wsc.png")));

    MainWindow w;
    w.show();
    w.initialise();
    return QApplication::exec();
}
