#pragma once

#include <QMainWindow>
#include <QString>

#include "protocol.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class DeviceController;
class LogBuffer;
class LogViewerDialog;
class QLabel;
class QTimer;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    enum class BannerKind { Info, Warning, Error };
    void showBanner(const QString& message, BannerKind kind);
    void hideBanner();

    void initialise();
    void scanAndConnect();

    DeviceController* controller() const { return controller_; }

private slots:
    // Connection lifecycle
    void onConnected();
    void onDisconnected();
    void onProtocolMismatch(const QString& message);
    void onFatalIncompatibility(const QString& message);

    // Status tab (Ph3)
    void onStatusReceived(const System_Ready_Status_t& status);
    void onRtcReceived(const RTC_DateTime_t& dt);
    void onRtcWriteAck(bool ok);
    void onUpdateRtcClicked();
    void onClearDbClicked();
    void onDbFlushAck(bool ok);

    // Current Measurement tab (Ph4)
    void onWeatherReceived(const Weather_Data_Packed_t& weather);

    // General Settings tab (Ph5)
    void onMetaReceived(const Meta_Data_t& meta);
    void onMetaWriteAck(bool ok, quint8 nakCode);
    void onGenDiscardClicked();
    void onGenApplyClicked();
    void markGenDirty();

    // Sensor Settings tab (Ph6)
    void onSensorDiscardClicked();
    void onSensorApplyClicked();
    void markSensorDirty();

    // Tab management
    void onCurrentTabChanged(int index);

    // Debug Log dialog (Ph8)
    void onDebugLogAction();

    // Clock tick
    void updateComputerClock();

private:
    void resetToDisconnected();
    void populateGenFromMeta(const Meta_Data_t& meta);
    void populateSensorFromMeta(const Meta_Data_t& meta);
    static void setLed(QLabel* led, bool ok);

    Ui::MainWindow*   ui_           = nullptr;
    DeviceController* controller_   = nullptr;
    LogBuffer*        log_buffer_   = nullptr;
    LogViewerDialog*  log_dialog_   = nullptr;
    QTimer*           clockTimer_   = nullptr;
    QTimer*           statusPoll_   = nullptr;
    QTimer*           weatherPoll_  = nullptr;

    bool connected_      = false;
    bool genDirty_       = false;
    bool sensorDirty_    = false;
    bool tabChangeLock_  = false;
    int  prevTab_        = 0;
};
