// mainwindow.cpp — top-level window implementation (smoke-test stage).

#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QPushButton>
#include <QSerialPortInfo>
#include <QString>
#include <QStringList>

#include "app_info.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui_(new Ui::MainWindow) {
    ui_->setupUi(this);

    setWindowTitle(QStringLiteral("%1 v%2")
                       .arg(AppInfo::kAppDisplayName)
                       .arg(AppInfo::kAppVersion));

    connect(ui_->scanButton, &QPushButton::clicked,
            this, &MainWindow::onScanDevicesClicked);

    // Run an initial scan so the user sees something on first launch.
    onScanDevicesClicked();
}

MainWindow::~MainWindow() {
    delete ui_;
}

void MainWindow::onScanDevicesClicked() {
    ui_->deviceList->clear();

    const auto ports = QSerialPortInfo::availablePorts();
    QStringList matches;
    QStringList allPorts;

    for (const QSerialPortInfo& info : ports) {
        const QString line = QStringLiteral("%1   VID=0x%2 PID=0x%3   %4")
                                 .arg(info.portName())
                                 .arg(info.vendorIdentifier(),  4, 16, QChar('0'))
                                 .arg(info.productIdentifier(), 4, 16, QChar('0'))
                                 .arg(info.description().isEmpty()
                                          ? QStringLiteral("(no description)")
                                          : info.description());
        allPorts << line;

        if (info.hasVendorIdentifier() &&
            info.hasProductIdentifier() &&
            info.vendorIdentifier()  == AppInfo::kUsbVid &&
            info.productIdentifier() == AppInfo::kUsbPid) {
            matches << line;
        }
    }

    if (matches.isEmpty()) {
        ui_->deviceList->addItem(QStringLiteral(
            "No Robin Weather Station devices found (VID=0x%1 PID=0x%2).")
                .arg(AppInfo::kUsbVid, 4, 16, QChar('0'))
                .arg(AppInfo::kUsbPid, 4, 16, QChar('0')));
        ui_->deviceList->addItem(QString());
        ui_->deviceList->addItem(QStringLiteral("All serial ports detected:"));
        if (allPorts.isEmpty()) {
            ui_->deviceList->addItem(QStringLiteral("  (none)"));
        } else {
            for (const QString& s : allPorts) {
                ui_->deviceList->addItem(QStringLiteral("  ") + s);
            }
        }
    } else {
        ui_->deviceList->addItem(QStringLiteral(
            "Found %1 Robin Weather Station device(s):").arg(matches.size()));
        for (const QString& s : matches) {
            ui_->deviceList->addItem(QStringLiteral("  ") + s);
        }
    }
}
