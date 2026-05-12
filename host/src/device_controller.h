// device_controller.h — UI-thread owner of the device connection.
//
// DeviceController lives on the UI thread. It owns the IO thread (a QThread
// running a DeviceIO worker) and translates between UI actions and worker
// signals/slots. All communication with DeviceIO uses Qt::QueuedConnection.
//
// SCAFFOLD: methods are declared but not yet implemented. Implement as
// features land per CLAUDE.md.

#pragma once

#include <QObject>
#include <QSerialPortInfo>
#include <QByteArray>

class QThread;
class DeviceIO;

class DeviceController : public QObject {
    Q_OBJECT

public:
    explicit DeviceController(QObject* parent = nullptr);
    ~DeviceController() override;

    DeviceController(const DeviceController&)            = delete;
    DeviceController& operator=(const DeviceController&) = delete;

    // Enumerate plugged-in Robin Weather Station devices.
    static QList<QSerialPortInfo> findDevices();

public slots:
    void connectTo(const QSerialPortInfo& info);
    void disconnect();
    void sendCommand(quint8 opcode, const QByteArray& payload);

signals:
    void connected();
    void disconnectedSignal();
    void errorOccurred(const QString& message);
    void frameReceived(quint8 opcode, const QByteArray& payload);
    void protocolMismatch(const QString& message);
    void fatalIncompatibility(const QString& message);

private:
    QThread*  io_thread_ = nullptr;
    DeviceIO* io_worker_ = nullptr;  // lives on io_thread_
};
