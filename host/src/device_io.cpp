// device_io.cpp — IO worker; lives on the dedicated IO thread.

#include "device_io.h"

#include "app_info.h"
#include "protocol.h"

DeviceIO::DeviceIO(QObject* parent) : QObject(parent)
{
    connect(&port_, &QSerialPort::readyRead,
            this, &DeviceIO::onReadyRead);
    connect(&port_, &QSerialPort::errorOccurred,
            this, &DeviceIO::onSerialError);
}

DeviceIO::~DeviceIO()
{
    if (port_.isOpen()) {
        port_.close();
    }
}

void DeviceIO::openPort(const QSerialPortInfo& info)
{
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

    // Suppress spurious MCU resets that some boards trigger on DTR/RTS edges.
    port_.setDataTerminalReady(false);
    port_.setRequestToSend(false);

    parser_.reset();
    emit opened();
}

void DeviceIO::closePort()
{
    if (port_.isOpen()) {
        port_.close();
    }
    emit closed();
}

void DeviceIO::sendCommand(quint8 opcode, const QByteArray& payload)
{
    if (!port_.isOpen()) {
        return;
    }

    // Build Host→Device frame: MAGIC_H2D | cmd | payload | FOOTER_H2D
    QByteArray frame;
    frame.reserve(2 + 1 + payload.size() + 2);
    frame += static_cast<char>(ROBIN_MAGIC_H2D_H);
    frame += static_cast<char>(ROBIN_MAGIC_H2D_L);
    frame += static_cast<char>(opcode);
    frame += payload;
    frame += static_cast<char>(ROBIN_FOOTER_H2D_H);
    frame += static_cast<char>(ROBIN_FOOTER_H2D_L);

    port_.write(frame);
}

void DeviceIO::onReadyRead()
{
    const QByteArray chunk = port_.readAll();
    parser_.feed(chunk, [this](quint8 op, const QByteArray& pay) {
        emit frameReceived(op, pay);
    });
}

void DeviceIO::onSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError) {
        return;
    }
    emit errorOccurred(port_.errorString());
}
