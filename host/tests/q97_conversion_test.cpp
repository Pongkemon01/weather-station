// q97_conversion_test.cpp — Unit tests for Q9.7 fixed-point helpers in protocol.h.

#include <QtTest>
#include "protocol.h"

class Q97ConversionTest : public QObject {
    Q_OBJECT

private slots:
    void toFloat_zero()     { QCOMPARE(robin_fixedpt_to_float(0),    0.0f); }
    void toFloat_one()      { QCOMPARE(robin_fixedpt_to_float(128),  1.0f); }
    void toFloat_negOne()   { QCOMPARE(robin_fixedpt_to_float(-128), -1.0f); }
    void toFloat_half()     { QCOMPARE(robin_fixedpt_to_float(64),   0.5f); }
    void toFloat_negHalf()  { QCOMPARE(robin_fixedpt_to_float(-64),  -0.5f); }

    void roundTrip_positive() {
        float v = 23.5f;
        QCOMPARE(robin_fixedpt_to_float(robin_float_to_fixedpt(v)), v);
    }
    void roundTrip_negative() {
        float v = -5.5f;
        QCOMPARE(robin_fixedpt_to_float(robin_float_to_fixedpt(v)), v);
    }
    void roundTrip_zero() {
        QCOMPARE(robin_fixedpt_to_float(robin_float_to_fixedpt(0.0f)), 0.0f);
    }

    void saturation_max() {
        QCOMPARE(robin_float_to_fixedpt(1000.0f), int16_t{32767});
    }
    void saturation_min() {
        QCOMPARE(robin_float_to_fixedpt(-1000.0f), int16_t{-32768});
    }

    void typical_temperature() {
        // 25.25 degC → wire value 3232 → back to 25.25
        auto wire = robin_float_to_fixedpt(25.25f);
        QCOMPARE(robin_fixedpt_to_float(wire), 25.25f);
    }
    void typical_humidity() {
        auto wire = robin_float_to_fixedpt(60.0f);
        QCOMPARE(robin_fixedpt_to_float(wire), 60.0f);
    }
};

QTEST_MAIN(Q97ConversionTest)
#include "q97_conversion_test.moc"
