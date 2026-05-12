// device_io.h — worker living on the IO thread.
//
// DeviceIO owns the QSerialPort and the FrameParser. All blocking USB-CDC
// activity happens on its thread; results are emitted via queued signals.
//
// SCAFFOLD: structure only. Implement per CLAUDE.md.

#pragma once

#include <QObject>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QByteArray>

#include "frame_parser.h"

class DeviceIO : public QObject {
    Q_OBJECT

public:
    explicit DeviceIO(QObject* parent = nullptr);
    ~DeviceIO() override;

    DeviceIO(const DeviceIO&)            = delete;
    DeviceIO& operator=(const DeviceIO&) = delete;

public slots:
    void openPort(const QSerialPortInfo& info);
    void closePort();
    void sendCommand(quint8 opcode, const QByteArray& payload);

signals:
    void opened();
    void closed();
    void errorOccurred(const QString& message);
    void frameReceived(quint8 opcode, const QByteArray& payload);

private slots:
    void onReadyRead();
    void onSerialError(QSerialPort::SerialPortError error);

private:
    QSerialPort  port_;
    FrameParser  parser_;
};
