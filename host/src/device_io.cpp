// device_io.cpp — IO worker (scaffold).

#include "device_io.h"

#include "app_info.h"

DeviceIO::DeviceIO(QObject* parent) : QObject(parent) {
    connect(&port_, &QSerialPort::readyRead, this, &DeviceIO::onReadyRead);
    connect(&port_, &QSerialPort::errorOccurred,
            this, &DeviceIO::onSerialError);
}

DeviceIO::~DeviceIO() {
    if (port_.isOpen()) {
        port_.close();
    }
}

void DeviceIO::openPort(const QSerialPortInfo& info) {
    if (port_.isOpen()) {
        port_.close();
    }

    port_.setPort(info);
    port_.setBaudRate(AppInfo::kSerialBaudRate);
    port_.setDataBits(QSerialPort::Data8);
    port_.setParity(QSerialPort::NoParity);
    port_.setStopBits(QSerialPort::OneStop);
    port_.setFlowControl(QSerialPort::NoFlowControl);

    if (!port_.open(QIODevice::ReadWrite)) {
        emit errorOccurred(port_.errorString());
        return;
    }

    // Avoid surprise resets on MCUs that interpret DTR/RTS toggling.
    port_.setDataTerminalReady(false);
    port_.setRequestToSend(false);

    parser_.reset();
    emit opened();
}

void DeviceIO::closePort() {
    if (port_.isOpen()) {
        port_.close();
    }
    emit closed();
}

void DeviceIO::sendCommand(quint8 opcode, const QByteArray& payload) {
    Q_UNUSED(opcode);
    Q_UNUSED(payload);
    // TODO: frame the command per shared/protocol.h, then port_.write(...).
}

void DeviceIO::onReadyRead() {
    const QByteArray chunk = port_.readAll();
    // TODO: feed chunk into parser_, emit frameReceived for each complete frame.
    Q_UNUSED(chunk);
}

void DeviceIO::onSerialError(QSerialPort::SerialPortError error) {
    if (error == QSerialPort::NoError) {
        return;
    }
    emit errorOccurred(port_.errorString());
}
