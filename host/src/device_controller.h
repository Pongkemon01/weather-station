// device_controller.h — UI-thread owner of the device connection.
//
// All communication with DeviceIO uses Qt::QueuedConnection.
// Typed request slots emit typed completion signals; one in-flight at a time.

#pragma once

#include <QObject>
#include <QSerialPortInfo>
#include <QByteArray>

#include "protocol.h"

class QThread;
class QTimer;
class DeviceIO;
class LogBuffer;

class DeviceController : public QObject {
    Q_OBJECT

public:
    explicit DeviceController(QObject* parent = nullptr);
    ~DeviceController() override;

    DeviceController(const DeviceController&)            = delete;
    DeviceController& operator=(const DeviceController&) = delete;

    static QList<QSerialPortInfo> findDevices();

    void setLogBuffer(LogBuffer* log) { log_ = log; }

    bool               isMetaCacheValid() const { return meta_cache_valid_; }
    const Meta_Data_t& metaCache()        const { return meta_cache_; }
    void               invalidateMetaCache();

public slots:
    void connectTo(const QSerialPortInfo& info);
    void disconnect();

    void requestStatus();
    void requestWeather();
    void requestMeta();
    void setMeta(const Meta_Data_t& meta);
    void setRtc(const RTC_DateTime_t& dt);
    void requestRtc();
    void clearDatabase();

signals:
    void connected();
    void disconnectedSignal();
    void errorOccurred(const QString& message);
    void frameReceived(quint8 opcode, const QByteArray& payload);  // raw pass-through
    void protocolMismatch(const QString& message);
    void fatalIncompatibility(const QString& message);

    // Typed responses.
    void statusReceived(const System_Ready_Status_t& status);
    void weatherReceived(const Weather_Data_Packed_t& weather);
    void metaReceived(const Meta_Data_t& meta);
    void metaWriteAck(bool ok, quint8 nakCode);
    void rtcReceived(const RTC_DateTime_t& dt);
    void rtcWriteAck(bool ok);
    void dbFlushAck(bool ok);
    void requestTimedOut(quint8 opcode);

private slots:
    void onRawFrame(quint8 opcode, const QByteArray& payload);
    void onTimeout();

private:
    void sendRawCommand(quint8 opcode, const QByteArray& payload = {});
    void armTimeout(quint8 opcode);
    void clearTimeout();
    void logMsg(const QString& msg);

    QThread*    io_thread_        = nullptr;
    DeviceIO*   io_worker_        = nullptr;
    QTimer*     timeout_timer_    = nullptr;
    quint8      pending_op_       = 0;
    Meta_Data_t meta_cache_       = {};
    bool        meta_cache_valid_ = false;
    LogBuffer*  log_              = nullptr;
};
