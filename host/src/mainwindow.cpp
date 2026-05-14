#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <cstring>
#include <QDateTime>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QTimer>

#include "app_info.h"
#include "device_controller.h"
#include "dialogs/device_picker_dialog.h"
#include "dialogs/log_viewer_dialog.h"
#include "log_buffer.h"
#include "protocol.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui_(new Ui::MainWindow)
{
    ui_->setupUi(this);

    setWindowTitle(tr("%1 v%2")
                       .arg(QLatin1String(AppInfo::kAppDisplayName))
                       .arg(QLatin1String(AppInfo::kAppVersion)));
    setWindowIcon(QIcon(QStringLiteral(":/icons/robin_wsc.png")));

    ui_->aboutLogoLabel->setPixmap(
        QPixmap(QStringLiteral(":/icons/robin_wsc.png")));
    ui_->aboutVersionLabel->setText(
        tr("Version %1").arg(QLatin1String(AppInfo::kAppVersion)));

    // ----- Log buffer --------------------------------------------------------
    log_buffer_ = new LogBuffer(this, AppInfo::kLogBufferCapacity);

    // ----- Controller --------------------------------------------------------
    controller_ = new DeviceController(this);
    controller_->setLogBuffer(log_buffer_);

    // ----- Polling timers ----------------------------------------------------
    statusPoll_ = new QTimer(this);
    statusPoll_->setInterval(2000);
    connect(statusPoll_, &QTimer::timeout,
            controller_, &DeviceController::requestStatus);

    weatherPoll_ = new QTimer(this);
    weatherPoll_->setInterval(1000);
    connect(weatherPoll_, &QTimer::timeout,
            controller_, &DeviceController::requestWeather);

    // ----- Clock timer -------------------------------------------------------
    clockTimer_ = new QTimer(this);
    clockTimer_->setInterval(1000);
    connect(clockTimer_, &QTimer::timeout, this, &MainWindow::updateComputerClock);
    clockTimer_->start();
    updateComputerClock();

    // ----- Controller signals ------------------------------------------------
    connect(controller_, &DeviceController::connected,
            this, &MainWindow::onConnected);
    connect(controller_, &DeviceController::disconnectedSignal,
            this, &MainWindow::onDisconnected);
    connect(controller_, &DeviceController::protocolMismatch,
            this, &MainWindow::onProtocolMismatch);
    connect(controller_, &DeviceController::fatalIncompatibility,
            this, &MainWindow::onFatalIncompatibility);

    connect(controller_, &DeviceController::statusReceived,
            this, &MainWindow::onStatusReceived);
    connect(controller_, &DeviceController::rtcReceived,
            this, &MainWindow::onRtcReceived);
    connect(controller_, &DeviceController::rtcWriteAck,
            this, &MainWindow::onRtcWriteAck);
    connect(controller_, &DeviceController::weatherReceived,
            this, &MainWindow::onWeatherReceived);
    connect(controller_, &DeviceController::metaReceived,
            this, &MainWindow::onMetaReceived);
    connect(controller_, &DeviceController::metaWriteAck,
            this, &MainWindow::onMetaWriteAck);
    connect(controller_, &DeviceController::dbFlushAck,
            this, &MainWindow::onDbFlushAck);

    // ----- Status tab --------------------------------------------------------
    connect(ui_->statusReconnectButton, &QPushButton::clicked,
            this, &MainWindow::scanAndConnect);
    connect(ui_->statusUpdateRtcButton, &QPushButton::clicked,
            this, &MainWindow::onUpdateRtcClicked);
    connect(ui_->statusClearDbButton, &QPushButton::clicked,
            this, &MainWindow::onClearDbClicked);

    // ----- General Settings dirty tracking -----------------------------------
    connect(ui_->genRegionIdSpin,        QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::markGenDirty);
    connect(ui_->genStationIdSpin,       QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::markGenDirty);
    connect(ui_->genSamplingIntervalSpin,QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::markGenDirty);
    connect(ui_->genServerDnsEdit,       &QLineEdit::textEdited,
            this, &MainWindow::markGenDirty);
    connect(ui_->genSensorPathEdit,      &QLineEdit::textEdited,
            this, &MainWindow::markGenDirty);
    connect(ui_->genFirmwarePathEdit,    &QLineEdit::textEdited,
            this, &MainWindow::markGenDirty);
    connect(ui_->genDiscardButton,       &QPushButton::clicked,
            this, &MainWindow::onGenDiscardClicked);
    connect(ui_->genApplyButton,         &QPushButton::clicked,
            this, &MainWindow::onGenApplyClicked);

    // ----- Sensor Settings dirty tracking ------------------------------------
    connect(ui_->sensorTempAdjSpin,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::markSensorDirty);
    connect(ui_->sensorHumidityAdjSpin,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::markSensorDirty);
    connect(ui_->sensorPressureAdjSpin,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::markSensorDirty);
    connect(ui_->sensorLightAdjSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::markSensorDirty);
    connect(ui_->sensorRainfallAdjSpin,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::markSensorDirty);
    connect(ui_->sensorDiscardButton, &QPushButton::clicked,
            this, &MainWindow::onSensorDiscardClicked);
    connect(ui_->sensorApplyButton,   &QPushButton::clicked,
            this, &MainWindow::onSensorApplyClicked);

    // ----- Tab switching -----------------------------------------------------
    connect(ui_->tabWidget, &QTabWidget::currentChanged,
            this, &MainWindow::onCurrentTabChanged);

    // ----- Help menu ---------------------------------------------------------
    connect(ui_->actionDebugLog, &QAction::triggered,
            this, &MainWindow::onDebugLogAction);

    resetToDisconnected();
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
    if (connected_)
        controller_->disconnect();

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

// ---- Connection lifecycle ---------------------------------------------------

void MainWindow::onConnected()
{
    connected_ = true;
    hideBanner();
    ui_->statusbar->showMessage(tr("Connected"));

    ui_->statusConnectionLabel->setText(tr("Connected"));
    ui_->statusConnectionLabel->setStyleSheet(
        QStringLiteral("color: #00aa00; font-weight: bold;"));

    ui_->statusUpdateRtcButton->setEnabled(true);
    ui_->statusClearDbButton->setEnabled(true);

    ui_->genRegionIdSpin->setEnabled(true);
    ui_->genStationIdSpin->setEnabled(true);
    ui_->genSamplingIntervalSpin->setEnabled(true);
    ui_->genServerDnsEdit->setEnabled(true);
    ui_->genSensorPathEdit->setEnabled(true);
    ui_->genFirmwarePathEdit->setEnabled(true);
    ui_->genDiscardButton->setEnabled(true);
    ui_->genApplyButton->setEnabled(true);

    ui_->sensorTempAdjSpin->setEnabled(true);
    ui_->sensorHumidityAdjSpin->setEnabled(true);
    ui_->sensorPressureAdjSpin->setEnabled(true);
    ui_->sensorLightAdjSpin->setEnabled(true);
    ui_->sensorRainfallAdjSpin->setEnabled(true);
    ui_->sensorDiscardButton->setEnabled(true);
    ui_->sensorApplyButton->setEnabled(true);

    // Kick off initial reads.
    controller_->requestRtc();
    controller_->requestMeta();

    // Start polling for the currently-visible tab.
    int tab = ui_->tabWidget->currentIndex();
    if (tab == 0) statusPoll_->start();
    if (tab == 1) weatherPoll_->start();

    log_buffer_->info(QStringLiteral("Device connected"));
}

void MainWindow::onDisconnected()
{
    connected_ = false;
    controller_->invalidateMetaCache();
    resetToDisconnected();
    ui_->statusbar->showMessage(tr("Disconnected"));
    showBanner(tr("Device disconnected. Connect the device and use Re-connect."),
               BannerKind::Warning);
    log_buffer_->info(QStringLiteral("Device disconnected"));
}

void MainWindow::onProtocolMismatch(const QString& message)
{
    showBanner(message, BannerKind::Warning);
}

void MainWindow::onFatalIncompatibility(const QString& message)
{
    QMessageBox::critical(this, tr("Fatal Incompatibility"), message);
    for (int i = 1; i < ui_->tabWidget->count(); ++i)
        ui_->tabWidget->setTabEnabled(i, false);
}

// ---- Banner -----------------------------------------------------------------

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

// ---- Clock ------------------------------------------------------------------

void MainWindow::updateComputerClock()
{
    ui_->statusComputerTimeValue->setText(
        QDateTime::currentDateTime().toString(
            QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}

// ---- Status tab (Ph3) -------------------------------------------------------

// static
void MainWindow::setLed(QLabel* led, bool ok)
{
    led->setStyleSheet(ok
        ? QStringLiteral("background-color: #00aa00; border-radius: 3px;")
        : QStringLiteral("background-color: #dd3333; border-radius: 3px;"));
}

void MainWindow::onStatusReceived(const System_Ready_Status_t& st)
{
    setLed(ui_->statusLcdLed,      st.ui_ready        != 0);
    setLed(ui_->statusRtcLed,      st.datetime_ready  != 0);
    setLed(ui_->statusModbusLed,   st.modbus_ready    != 0);
    setLed(ui_->statusTemphumLed,  st.sht45_ready     != 0);
    setLed(ui_->statusLightLed,    st.light_ok        != 0);
    setLed(ui_->statusSdLed,       st.sd_detected     != 0);
    setLed(ui_->statusNvramLed,    st.fram_ready      != 0);
    setLed(ui_->statusUsartLed,    st.usart_ready     != 0);
    setLed(ui_->statusLteLed,      st.a7670_ready     != 0);
    setLed(ui_->statusPressureLed, st.bmp390_ready    != 0);
    setLed(ui_->statusRainLed,     st.rainfall_ok     != 0);
    // sd_write_protected: 0 = not protected = good (green)
    setLed(ui_->statusSdwpLed,     st.sd_write_protected == 0);
}

void MainWindow::onRtcReceived(const RTC_DateTime_t& dt)
{
    ui_->statusDeviceTimeValue->setText(
        QStringLiteral("%1-%2-%3 %4:%5:%6")
        .arg(2000 + dt.year,  4, 10, QChar('0'))
        .arg(dt.month,        2, 10, QChar('0'))
        .arg(dt.day,          2, 10, QChar('0'))
        .arg(dt.hours,        2, 10, QChar('0'))
        .arg(dt.minutes,      2, 10, QChar('0'))
        .arg(dt.seconds,      2, 10, QChar('0')));
}

void MainWindow::onRtcWriteAck(bool ok)
{
    if (ok)
        controller_->requestRtc();
    else
        showBanner(tr("Failed to update device RTC."), BannerKind::Error);
}

void MainWindow::onUpdateRtcClicked()
{
    QDateTime now = QDateTime::currentDateTime();
    RTC_DateTime_t dt{};
    dt.year    = static_cast<uint8_t>(now.date().year() - 2000);
    dt.month   = static_cast<uint8_t>(now.date().month());
    dt.day     = static_cast<uint8_t>(now.date().day());
    dt.hours   = static_cast<uint8_t>(now.time().hour());
    dt.minutes = static_cast<uint8_t>(now.time().minute());
    dt.seconds = static_cast<uint8_t>(now.time().second());
    controller_->setRtc(dt);
}

void MainWindow::onClearDbClicked()
{
    auto ret = QMessageBox::warning(
        this, tr("Clear Database"),
        tr("This will permanently erase all stored weather data on the device. Continue?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret == QMessageBox::Yes)
        controller_->clearDatabase();
}

void MainWindow::onDbFlushAck(bool ok)
{
    if (ok)
        showBanner(tr("Database cleared successfully."), BannerKind::Info);
    else
        showBanner(tr("Failed to clear database."), BannerKind::Error);
}

// ---- Current Measurement tab (Ph4) -----------------------------------------

void MainWindow::onWeatherReceived(const Weather_Data_Packed_t& wd)
{
    auto setVal = [this](int row, const QString& val) {
        ui_->measTable->item(row, 1)->setText(val);
    };

    auto fp = [](int16_t v) {
        return QString::number(static_cast<double>(robin_fixedpt_to_float(v)), 'f', 2);
    };

    setVal(0, fp(wd.temperature));
    setVal(1, fp(wd.humidity));
    setVal(2, fp(wd.pressure));
    setVal(3, QString::number(wd.light_par));
    setVal(4, fp(wd.rainfall));
    setVal(5, fp(wd.dew_point));
    setVal(6, fp(wd.bus_value));
}

// ---- General Settings tab (Ph5) --------------------------------------------

void MainWindow::populateGenFromMeta(const Meta_Data_t& meta)
{
    const QSignalBlocker b1(ui_->genRegionIdSpin);
    const QSignalBlocker b2(ui_->genStationIdSpin);
    const QSignalBlocker b3(ui_->genSamplingIntervalSpin);
    const QSignalBlocker b4(ui_->genServerDnsEdit);
    const QSignalBlocker b5(ui_->genSensorPathEdit);
    const QSignalBlocker b6(ui_->genFirmwarePathEdit);

    ui_->genRegionIdSpin->setValue(meta.region_id);
    ui_->genStationIdSpin->setValue(meta.station_id);
    ui_->genSamplingIntervalSpin->setValue(meta.sampling_interval);
    ui_->genServerDnsEdit->setText(QString::fromLatin1(meta.server_name));
    ui_->genSensorPathEdit->setText(QString::fromLatin1(meta.server_path));
    ui_->genFirmwarePathEdit->setText(QString::fromLatin1(meta.update_path));

    genDirty_ = false;
}

void MainWindow::onMetaReceived(const Meta_Data_t& meta)
{
    populateGenFromMeta(meta);
    populateSensorFromMeta(meta);
}

void MainWindow::onMetaWriteAck(bool ok, quint8 nakCode)
{
    if (ok) {
        genDirty_ = false;
        sensorDirty_ = false;
        controller_->requestMeta();
        showBanner(tr("Settings saved."), BannerKind::Info);
    } else {
        showBanner(tr("Failed to save settings (error 0x%1).")
                   .arg(nakCode, 2, 16, QChar('0')), BannerKind::Error);
    }
}

void MainWindow::markGenDirty()
{
    genDirty_ = true;
}

void MainWindow::onGenDiscardClicked()
{
    if (controller_->isMetaCacheValid()) {
        populateGenFromMeta(controller_->metaCache());
    } else {
        controller_->requestMeta();
    }
    genDirty_ = false;
}

void MainWindow::onGenApplyClicked()
{
    if (!controller_->isMetaCacheValid())
        return;

    Meta_Data_t meta = controller_->metaCache();  // copy — preserves calibration
    meta.region_id         = static_cast<uint16_t>(ui_->genRegionIdSpin->value());
    meta.station_id        = static_cast<uint16_t>(ui_->genStationIdSpin->value());
    meta.sampling_interval = static_cast<uint8_t>(ui_->genSamplingIntervalSpin->value());

    auto writeStr = [](char* dst, size_t dstsz, const QByteArray& src) {
        std::memset(dst, 0, dstsz);
        std::strncpy(dst, src.constData(),
                     static_cast<size_t>(dstsz - 1));
    };

    writeStr(meta.server_name, sizeof(meta.server_name),
             ui_->genServerDnsEdit->text().toLatin1());
    writeStr(meta.server_path, sizeof(meta.server_path),
             ui_->genSensorPathEdit->text().toLatin1());
    writeStr(meta.update_path, sizeof(meta.update_path),
             ui_->genFirmwarePathEdit->text().toLatin1());

    controller_->setMeta(meta);
}

// ---- Sensor Settings tab (Ph6) ---------------------------------------------

void MainWindow::populateSensorFromMeta(const Meta_Data_t& meta)
{
    const QSignalBlocker b1(ui_->sensorTempAdjSpin);
    const QSignalBlocker b2(ui_->sensorHumidityAdjSpin);
    const QSignalBlocker b3(ui_->sensorPressureAdjSpin);
    const QSignalBlocker b4(ui_->sensorLightAdjSpin);
    const QSignalBlocker b5(ui_->sensorRainfallAdjSpin);

    ui_->sensorTempAdjSpin->setValue(static_cast<double>(meta.temperature_adj));
    ui_->sensorHumidityAdjSpin->setValue(static_cast<double>(meta.humidity_adj));
    ui_->sensorPressureAdjSpin->setValue(static_cast<double>(meta.pressure_adj));
    ui_->sensorLightAdjSpin->setValue(meta.light_adj);
    ui_->sensorRainfallAdjSpin->setValue(static_cast<double>(meta.rainfall_adj));

    sensorDirty_ = false;
}

void MainWindow::markSensorDirty()
{
    sensorDirty_ = true;
}

void MainWindow::onSensorDiscardClicked()
{
    if (controller_->isMetaCacheValid()) {
        populateSensorFromMeta(controller_->metaCache());
    } else {
        controller_->requestMeta();
    }
    sensorDirty_ = false;
}

void MainWindow::onSensorApplyClicked()
{
    if (!controller_->isMetaCacheValid())
        return;

    Meta_Data_t meta = controller_->metaCache();  // copy — preserves general settings
    meta.temperature_adj = static_cast<float>(ui_->sensorTempAdjSpin->value());
    meta.humidity_adj    = static_cast<float>(ui_->sensorHumidityAdjSpin->value());
    meta.pressure_adj    = static_cast<float>(ui_->sensorPressureAdjSpin->value());
    meta.light_adj       = static_cast<int16_t>(ui_->sensorLightAdjSpin->value());
    meta.rainfall_adj    = static_cast<float>(ui_->sensorRainfallAdjSpin->value());

    controller_->setMeta(meta);
}

// ---- Tab management --------------------------------------------------------

void MainWindow::onCurrentTabChanged(int index)
{
    if (tabChangeLock_)
        return;

    // Check dirty state on leave.
    auto checkDirty = [&](int dirtyTab, bool& dirty, auto populateFn,
                           const QString& title) -> bool {
        if (prevTab_ == dirtyTab && dirty && index != dirtyTab) {
            auto ret = QMessageBox::question(
                this, title,
                tr("You have unsaved changes. Discard them?"),
                QMessageBox::Yes | QMessageBox::No);
            if (ret == QMessageBox::No) {
                tabChangeLock_ = true;
                ui_->tabWidget->setCurrentIndex(dirtyTab);
                tabChangeLock_ = false;
                return false;
            }
            dirty = false;
            if (controller_->isMetaCacheValid())
                populateFn(controller_->metaCache());
        }
        return true;
    };

    if (!checkDirty(2, genDirty_,
                    [this](const Meta_Data_t& m){ populateGenFromMeta(m); },
                    tr("Unsaved General Settings")))
        return;

    if (!checkDirty(3, sensorDirty_,
                    [this](const Meta_Data_t& m){ populateSensorFromMeta(m); },
                    tr("Unsaved Sensor Settings")))
        return;

    // Manage polling timers.
    if (index == 0) statusPoll_->start();   else statusPoll_->stop();
    if (index == 1) weatherPoll_->start();  else weatherPoll_->stop();

    // Load meta lazily when entering settings tabs.
    if ((index == 2 || index == 3) && connected_ &&
        !controller_->isMetaCacheValid()) {
        controller_->requestMeta();
    }

    prevTab_ = index;
}

// ---- Debug Log (Ph8) -------------------------------------------------------

void MainWindow::onDebugLogAction()
{
    if (!log_dialog_)
        log_dialog_ = new LogViewerDialog(log_buffer_, this);
    log_dialog_->show();
    log_dialog_->raise();
    log_dialog_->activateWindow();
}

// ---- Reset -----------------------------------------------------------------

void MainWindow::resetToDisconnected()
{
    connected_ = false;
    genDirty_  = false;
    sensorDirty_ = false;

    if (statusPoll_)  statusPoll_->stop();
    if (weatherPoll_) weatherPoll_->stop();

    const QString gray =
        QStringLiteral("background-color: #aaaaaa; border-radius: 3px;");
    ui_->statusLcdLed->setStyleSheet(gray);
    ui_->statusRtcLed->setStyleSheet(gray);
    ui_->statusModbusLed->setStyleSheet(gray);
    ui_->statusTemphumLed->setStyleSheet(gray);
    ui_->statusLightLed->setStyleSheet(gray);
    ui_->statusSdLed->setStyleSheet(gray);
    ui_->statusNvramLed->setStyleSheet(gray);
    ui_->statusUsartLed->setStyleSheet(gray);
    ui_->statusLteLed->setStyleSheet(gray);
    ui_->statusPressureLed->setStyleSheet(gray);
    ui_->statusRainLed->setStyleSheet(gray);
    ui_->statusSdwpLed->setStyleSheet(gray);

    ui_->statusConnectionLabel->setText(QStringLiteral("—"));
    ui_->statusConnectionLabel->setStyleSheet(QString());
    ui_->statusDeviceTimeValue->setText(QStringLiteral("—"));
    ui_->statusUpdateRtcButton->setEnabled(false);
    ui_->statusClearDbButton->setEnabled(false);

    const QString dash = QStringLiteral("—");
    for (int r = 0; r < ui_->measTable->rowCount(); ++r)
        ui_->measTable->item(r, 1)->setText(dash);

    ui_->genRegionIdSpin->setEnabled(false);
    ui_->genRegionIdSpin->setValue(0);
    ui_->genStationIdSpin->setEnabled(false);
    ui_->genStationIdSpin->setValue(0);
    ui_->genSamplingIntervalSpin->setEnabled(false);
    ui_->genSamplingIntervalSpin->setValue(10);
    ui_->genServerDnsEdit->setEnabled(false);
    ui_->genServerDnsEdit->clear();
    ui_->genSensorPathEdit->setEnabled(false);
    ui_->genSensorPathEdit->clear();
    ui_->genFirmwarePathEdit->setEnabled(false);
    ui_->genFirmwarePathEdit->clear();
    ui_->genDiscardButton->setEnabled(false);
    ui_->genApplyButton->setEnabled(false);

    ui_->sensorTempAdjSpin->setEnabled(false);
    ui_->sensorTempAdjSpin->setValue(0.0);
    ui_->sensorHumidityAdjSpin->setEnabled(false);
    ui_->sensorHumidityAdjSpin->setValue(0.0);
    ui_->sensorPressureAdjSpin->setEnabled(false);
    ui_->sensorPressureAdjSpin->setValue(0.0);
    ui_->sensorLightAdjSpin->setEnabled(false);
    ui_->sensorLightAdjSpin->setValue(0);
    ui_->sensorRainfallAdjSpin->setEnabled(false);
    ui_->sensorRainfallAdjSpin->setValue(0.0);
    ui_->sensorDiscardButton->setEnabled(false);
    ui_->sensorApplyButton->setEnabled(false);
}
