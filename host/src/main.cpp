// main.cpp — application entry point.
//
// Sets up QApplication identity from AppInfo, then opens the main window.

#include <QApplication>

#include "app_info.h"
#include "mainwindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QApplication::setApplicationName(AppInfo::kAppDisplayName);
    QApplication::setApplicationDisplayName(AppInfo::kAppDisplayName);
    QApplication::setApplicationVersion(AppInfo::kAppVersion);
    QApplication::setOrganizationName(AppInfo::kOrganizationName);
    QApplication::setOrganizationDomain(AppInfo::kOrganizationDomain);

    MainWindow w;
    w.show();
    return QApplication::exec();
}
