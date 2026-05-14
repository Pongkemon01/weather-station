// device_controller.cpp — UI-thread controller; owns the IO thread.

#include "device_controller.h"

#include <cstring>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QSerialPortInfo>

#include "app_info.h"
#include "device_io.h"
#include "log_buffer.h"
#include "protocol.h"

static constexpr int kTimeoutMs = 1500;

DeviceController::DeviceController(QObject* parent) : QObject(parent)
{
    io_worker_ = new DeviceIO;
    io_thread_ = new QThread(this);

    io_worker_->moveToThread(io_thread_);

    connect(io_thread_, &QThread::finished,
            io_worker_, &QObject::deleteLater);

    connect(io_worker_, &DeviceIO::opened,
            this, &DeviceController::connected,
            Qt::QueuedConnection);
    connect(io_worker_, &DeviceIO::closed,
            this, &DeviceController::disconnectedSignal,
            Qt::QueuedConnection);
    connect(io_worker_, &DeviceIO::errorOccurred,
            this, &DeviceController::errorOccurred,
            Qt::QueuedConnection);
    connect(io_worker_, &DeviceIO::frameReceived,
            this, &DeviceController::onRawFrame,
            Qt::QueuedConnection);

    timeout_timer_ = new QTimer(this);
    timeout_timer_->setSingleShot(true);
    timeout_timer_->setInterval(kTimeoutMs);
    connect(timeout_timer_, &QTimer::timeout, this, &DeviceController::onTimeout);

    io_thread_->start();
}

DeviceController::~DeviceController()
{
    io_thread_->quit();
    io_thread_->wait();
}

// static
QList<QSerialPortInfo> DeviceController::findDevices()
{
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

void DeviceController::invalidateMetaCache()
{
    meta_cache_valid_ = false;
}

void DeviceController::connectTo(const QSerialPortInfo& info)
{
    logMsg(QStringLiteral("Connecting to %1").arg(info.portName()));
    QMetaObject::invokeMethod(io_worker_, [w = io_worker_, info]() {
        w->openPort(info);
    }, Qt::QueuedConnection);
}

void DeviceController::disconnect()
{
    logMsg(QStringLiteral("Disconnecting"));
    invalidateMetaCache();
    clearTimeout();
    QMetaObject::invokeMethod(io_worker_, [w = io_worker_]() {
        w->closePort();
    }, Qt::QueuedConnection);
}

void DeviceController::requestStatus()
{
    logMsg(QStringLiteral("-> REQ_STATUS"));
    sendRawCommand(static_cast<quint8>(ROBIN_OP_REQ_STATUS));
    armTimeout(static_cast<quint8>(ROBIN_OP_REQ_STATUS));
}

void DeviceController::requestWeather()
{
    logMsg(QStringLiteral("-> REQ_WEATHER"));
    sendRawCommand(static_cast<quint8>(ROBIN_OP_REQ_WEATHER));
    armTimeout(static_cast<quint8>(ROBIN_OP_REQ_WEATHER));
}

void DeviceController::requestMeta()
{
    logMsg(QStringLiteral("-> REQ_META"));
    sendRawCommand(static_cast<quint8>(ROBIN_OP_REQ_META));
    armTimeout(static_cast<quint8>(ROBIN_OP_REQ_META));
}

void DeviceController::setMeta(const Meta_Data_t& meta)
{
    logMsg(QStringLiteral("-> SET_META"));
    QByteArray payload(reinterpret_cast<const char*>(&meta),
                       static_cast<qsizetype>(sizeof(meta)));
    sendRawCommand(static_cast<quint8>(ROBIN_OP_SET_META), payload);
    armTimeout(static_cast<quint8>(ROBIN_OP_SET_META));
}

void DeviceController::setRtc(const RTC_DateTime_t& dt)
{
    logMsg(QStringLiteral("-> SET_RTC"));
    QByteArray payload(reinterpret_cast<const char*>(&dt),
                       static_cast<qsizetype>(sizeof(dt)));
    sendRawCommand(static_cast<quint8>(ROBIN_OP_SET_RTC), payload);
    armTimeout(static_cast<quint8>(ROBIN_OP_SET_RTC));
}

void DeviceController::requestRtc()
{
    logMsg(QStringLiteral("-> REQ_RTC"));
    sendRawCommand(static_cast<quint8>(ROBIN_OP_REQ_RTC));
    armTimeout(static_cast<quint8>(ROBIN_OP_REQ_RTC));
}

void DeviceController::clearDatabase()
{
    logMsg(QStringLiteral("-> DB_FLUSH"));
    sendRawCommand(static_cast<quint8>(ROBIN_OP_DB_FLUSH));
    armTimeout(static_cast<quint8>(ROBIN_OP_DB_FLUSH));
}

void DeviceController::onRawFrame(quint8 opcode, const QByteArray& payload)
{
    emit frameReceived(opcode, payload);

    switch (static_cast<robin_opcode_t>(opcode)) {

    case ROBIN_OP_REQ_STATUS:
        if (static_cast<size_t>(payload.size()) == sizeof(System_Ready_Status_t)) {
            System_Ready_Status_t st{};
            std::memcpy(&st, payload.constData(), sizeof(st));
            logMsg(QStringLiteral("<- STATUS (%1 B)").arg(payload.size()));
            clearTimeout();
            emit statusReceived(st);
        }
        break;

    case ROBIN_OP_REQ_WEATHER:
        if (static_cast<size_t>(payload.size()) == sizeof(Weather_Data_Packed_t)) {
            Weather_Data_Packed_t wd{};
            std::memcpy(&wd, payload.constData(), sizeof(wd));
            logMsg(QStringLiteral("<- WEATHER (%1 B)").arg(payload.size()));
            clearTimeout();
            emit weatherReceived(wd);
        }
        break;

    case ROBIN_OP_REQ_META:
        if (static_cast<size_t>(payload.size()) == sizeof(Meta_Data_t)) {
            Meta_Data_t md{};
            std::memcpy(&md, payload.constData(), sizeof(md));
            meta_cache_ = md;
            meta_cache_valid_ = true;
            logMsg(QStringLiteral("<- META (%1 B)").arg(payload.size()));
            clearTimeout();
            emit metaReceived(md);
        }
        break;

    case ROBIN_OP_REQ_RTC:
        if (static_cast<size_t>(payload.size()) == sizeof(RTC_DateTime_t)) {
            RTC_DateTime_t dt{};
            std::memcpy(&dt, payload.constData(), sizeof(dt));
            logMsg(QStringLiteral("<- RTC (%1 B)").arg(payload.size()));
            clearTimeout();
            emit rtcReceived(dt);
        }
        break;

    case ROBIN_OP_ACK:
        if (!payload.isEmpty()) {
            auto acked = static_cast<quint8>(payload.at(0));
            logMsg(QStringLiteral("<- ACK (cmd=0x%1)").arg(acked, 2, 16, QChar('0')));
            clearTimeout();
            switch (static_cast<robin_opcode_t>(acked)) {
            case ROBIN_OP_SET_META: emit metaWriteAck(true, 0); break;
            case ROBIN_OP_SET_RTC:  emit rtcWriteAck(true);     break;
            case ROBIN_OP_DB_FLUSH: emit dbFlushAck(true);      break;
            default: break;
            }
        }
        break;

    case ROBIN_OP_NAK:
        if (!payload.isEmpty()) {
            auto err = static_cast<quint8>(payload.at(0));
            auto was = pending_op_;
            logMsg(QStringLiteral("<- NAK (err=0x%1, pending=0x%2)")
                   .arg(err, 2, 16, QChar('0'))
                   .arg(was, 2, 16, QChar('0')));
            clearTimeout();
            switch (static_cast<robin_opcode_t>(was)) {
            case ROBIN_OP_SET_META: emit metaWriteAck(false, err); break;
            case ROBIN_OP_SET_RTC:  emit rtcWriteAck(false);       break;
            case ROBIN_OP_DB_FLUSH: emit dbFlushAck(false);        break;
            default: break;
            }
        }
        break;

    default:
        break;
    }
}

void DeviceController::onTimeout()
{
    auto op = pending_op_;
    pending_op_ = 0;
    logMsg(QStringLiteral("TIMEOUT (cmd=0x%1)").arg(op, 2, 16, QChar('0')));
    emit requestTimedOut(op);
}

void DeviceController::sendRawCommand(quint8 opcode, const QByteArray& payload)
{
    QMetaObject::invokeMethod(io_worker_, [w = io_worker_, opcode, payload]() {
        w->sendCommand(opcode, payload);
    }, Qt::QueuedConnection);
}

void DeviceController::armTimeout(quint8 opcode)
{
    pending_op_ = opcode;
    timeout_timer_->start();
}

void DeviceController::clearTimeout()
{
    timeout_timer_->stop();
    pending_op_ = 0;
}

void DeviceController::logMsg(const QString& msg)
{
    if (log_)
        log_->debug(msg);
}
