// mainwindow.h — top-level window.
//
// For v0.0.1 this is a minimal smoke-test window that lists matching USB-CDC
// devices via QSerialPortInfo. Real UI (connection panel, status, config tabs,
// log viewer) will be built out as features land.

#pragma once

#include <QMainWindow>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    // Smoke-test action: scan QSerialPortInfo for our VID/PID and report.
    void onScanDevicesClicked();

private:
    Ui::MainWindow* ui_;
};
