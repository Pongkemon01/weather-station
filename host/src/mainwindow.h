#pragma once

#include <QMainWindow>
#include <QString>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class DeviceController;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    enum class BannerKind { Info, Warning, Error };
    void showBanner(const QString& message, BannerKind kind);
    void hideBanner();

    // Called from main() after show(). Triggers initial device scan.
    void initialise();

    // Scans for devices and connects: 0→banner, 1→auto, 2+→picker dialog.
    // Phase 3 wires the Re-connect button to call disconnect() then this.
    void scanAndConnect();

    DeviceController* controller() const { return controller_; }

private slots:
    void onConnected();
    void onDisconnected();
    void onProtocolMismatch(const QString& message);
    void onFatalIncompatibility(const QString& message);

private:
    Ui::MainWindow*   ui_;
    DeviceController* controller_ = nullptr;
};
