#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QIcon>
#include <QMessageBox>

#include "app_info.h"
#include "device_controller.h"
#include "dialogs/device_picker_dialog.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui_(new Ui::MainWindow)
{
    ui_->setupUi(this);

    setWindowTitle(tr("%1 v%2")
                       .arg(QLatin1String(AppInfo::kAppDisplayName))
                       .arg(QLatin1String(AppInfo::kAppVersion)));
    setWindowIcon(QIcon(QStringLiteral(":/icons/robin_wsc.png")));

    controller_ = new DeviceController(this);

    connect(controller_, &DeviceController::connected,
            this, &MainWindow::onConnected);
    connect(controller_, &DeviceController::disconnectedSignal,
            this, &MainWindow::onDisconnected);
    connect(controller_, &DeviceController::protocolMismatch,
            this, &MainWindow::onProtocolMismatch);
    connect(controller_, &DeviceController::fatalIncompatibility,
            this, &MainWindow::onFatalIncompatibility);
}

MainWindow::~MainWindow()
{
    delete ui_;
}

void MainWindow::initialise()
{
    scanAndConnect();
}

void MainWindow::scanAndConnect()
{
    auto devices = DeviceController::findDevices();

    if (devices.size() == 1) {
        hideBanner();
        controller_->connectTo(devices.first());
    } else if (devices.isEmpty()) {
        showBanner(tr("No Robin Weather Station detected. "
                      "Connect the device and use Re-connect."),
                   BannerKind::Warning);
    } else {
        DevicePickerDialog dlg(devices, this);
        if (dlg.exec() == QDialog::Accepted) {
            hideBanner();
            controller_->connectTo(dlg.selectedDevice());
        }
    }
}

void MainWindow::onConnected()
{
    hideBanner();
    ui_->statusbar->showMessage(tr("Connected"));
}

void MainWindow::onDisconnected()
{
    ui_->statusbar->showMessage(tr("Disconnected"));
    showBanner(tr("Device disconnected. Connect the device and use Re-connect."),
               BannerKind::Warning);
}

void MainWindow::onProtocolMismatch(const QString& message)
{
    showBanner(message, BannerKind::Warning);
}

void MainWindow::onFatalIncompatibility(const QString& message)
{
    QMessageBox::critical(this, tr("Fatal Incompatibility"), message);
    // Disable all tabs except Status (index 0) — keep status interactive.
    for (int i = 1; i < ui_->tabWidget->count(); ++i)
        ui_->tabWidget->setTabEnabled(i, false);
}

void MainWindow::showBanner(const QString& message, BannerKind kind)
{
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

void MainWindow::hideBanner()
{
    ui_->bannerLabel->setVisible(false);
    ui_->bannerLabel->clear();
}
