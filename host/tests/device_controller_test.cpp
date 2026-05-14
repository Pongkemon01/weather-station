// device_controller_test.cpp — Tests for DeviceController frame routing.
//
// We inject frames directly via invokeMethod on the private slot onRawFrame
// to test signal dispatch without a real USB device.

#include <QtTest>
#include <QSignalSpy>
#include <QMetaObject>

#include "device_controller.h"
#include "protocol.h"

class DeviceControllerTest : public QObject {
    Q_OBJECT

private slots:
    void init()    { dc_ = new DeviceController; }
    void cleanup() { delete dc_; dc_ = nullptr; }

    // ---- Ph2: typed frame routing -------------------------------------------

    void statusFrame_emitsStatusReceived() {
        System_Ready_Status_t st{};
        st.ui_ready = 1;
        QByteArray payload(reinterpret_cast<const char*>(&st),
                           static_cast<qsizetype>(sizeof(st)));

        QSignalSpy spy(dc_, &DeviceController::statusReceived);
        injectFrame(static_cast<quint8>(ROBIN_OP_REQ_STATUS), payload);

        QCOMPARE(spy.count(), 1);
    }

    void statusFrame_wrongSize_ignored() {
        QByteArray payload(3, 0);  // too short
        QSignalSpy spy(dc_, &DeviceController::statusReceived);
        injectFrame(static_cast<quint8>(ROBIN_OP_REQ_STATUS), payload);
        QCOMPARE(spy.count(), 0);
    }

    void weatherFrame_emitsWeatherReceived() {
        Weather_Data_Packed_t wd{};
        wd.temperature = 128;  // 1.0 °C in Q9.7
        QByteArray payload(reinterpret_cast<const char*>(&wd),
                           static_cast<qsizetype>(sizeof(wd)));

        QSignalSpy spy(dc_, &DeviceController::weatherReceived);
        injectFrame(static_cast<quint8>(ROBIN_OP_REQ_WEATHER), payload);

        QCOMPARE(spy.count(), 1);
    }

    void metaFrame_emitsMetaReceived_and_updatesCache() {
        Meta_Data_t md{};
        md.validation_value = ROBIN_META_VALIDATION_VALUE;
        md.region_id = 42;
        QByteArray payload(reinterpret_cast<const char*>(&md),
                           static_cast<qsizetype>(sizeof(md)));

        QSignalSpy spy(dc_, &DeviceController::metaReceived);
        QVERIFY(!dc_->isMetaCacheValid());

        injectFrame(static_cast<quint8>(ROBIN_OP_REQ_META), payload);

        QCOMPARE(spy.count(), 1);
        QVERIFY(dc_->isMetaCacheValid());
        QCOMPARE(dc_->metaCache().region_id, static_cast<uint16_t>(42));
    }

    void rtcFrame_emitsRtcReceived() {
        RTC_DateTime_t dt{};
        dt.year = 25; dt.month = 1; dt.day = 1;
        QByteArray payload(reinterpret_cast<const char*>(&dt),
                           static_cast<qsizetype>(sizeof(dt)));

        QSignalSpy spy(dc_, &DeviceController::rtcReceived);
        injectFrame(static_cast<quint8>(ROBIN_OP_REQ_RTC), payload);

        QCOMPARE(spy.count(), 1);
    }

    void ackSetMeta_emitsMetaWriteAckTrue() {
        QByteArray payload;
        payload.append(static_cast<char>(ROBIN_OP_SET_META));

        QSignalSpy spy(dc_, &DeviceController::metaWriteAck);
        injectFrame(static_cast<quint8>(ROBIN_OP_ACK), payload);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), true);
    }

    void ackSetRtc_emitsRtcWriteAckTrue() {
        QByteArray payload;
        payload.append(static_cast<char>(ROBIN_OP_SET_RTC));

        QSignalSpy spy(dc_, &DeviceController::rtcWriteAck);
        injectFrame(static_cast<quint8>(ROBIN_OP_ACK), payload);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), true);
    }

    void ackDbFlush_emitsDbFlushAckTrue() {
        QByteArray payload;
        payload.append(static_cast<char>(ROBIN_OP_DB_FLUSH));

        QSignalSpy spy(dc_, &DeviceController::dbFlushAck);
        injectFrame(static_cast<quint8>(ROBIN_OP_ACK), payload);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), true);
    }

    void rawFrameSignal_alwaysEmitted() {
        QByteArray payload(1, 0);
        QSignalSpy spy(dc_, &DeviceController::frameReceived);
        injectFrame(static_cast<quint8>(ROBIN_OP_ACK), payload);
        QCOMPARE(spy.count(), 1);
    }

    void invalidateMetaCache_clearsValid() {
        Meta_Data_t md{};
        QByteArray payload(reinterpret_cast<const char*>(&md),
                           static_cast<qsizetype>(sizeof(md)));
        injectFrame(static_cast<quint8>(ROBIN_OP_REQ_META), payload);
        QVERIFY(dc_->isMetaCacheValid());

        dc_->invalidateMetaCache();
        QVERIFY(!dc_->isMetaCacheValid());
    }

private:
    void injectFrame(quint8 opcode, const QByteArray& payload) {
        QMetaObject::invokeMethod(dc_, "onRawFrame",
            Qt::DirectConnection,
            Q_ARG(quint8, opcode),
            Q_ARG(QByteArray, payload));
    }

    DeviceController* dc_ = nullptr;
};

QTEST_MAIN(DeviceControllerTest)
#include "device_controller_test.moc"
