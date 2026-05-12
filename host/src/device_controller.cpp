// device_controller.cpp — UI-thread controller (scaffold).

#include "device_controller.h"

#include <QSerialPortInfo>

#include "app_info.h"
#include "device_io.h"

DeviceController::DeviceController(QObject* parent) : QObject(parent) {
    // TODO: create io_thread_, move io_worker_ to it, wire signals.
}

DeviceController::~DeviceController() {
    // TODO: shut down io_thread_ cleanly.
}

QList<QSerialPortInfo> DeviceController::findDevices() {
    QList<QSerialPortInfo> matches;
    for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
        if (info.hasVendorIdentifier() &&
            info.hasProductIdentifier() &&
            info.vendorIdentifier()  == AppInfo::kUsbVid &&
            info.productIdentifier() == AppInfo::kUsbPid) {
            matches.append(info);
        }
    }
    return matches;
}

void DeviceController::connectTo(const QSerialPortInfo& info) {
    Q_UNUSED(info);
    // TODO: post to io_worker_ via queued slot.
}

void DeviceController::disconnect() {
    // TODO: post to io_worker_ via queued slot.
}

void DeviceController::sendCommand(quint8 opcode, const QByteArray& payload) {
    Q_UNUSED(opcode);
    Q_UNUSED(payload);
    // TODO: post to io_worker_ via queued slot.
}
