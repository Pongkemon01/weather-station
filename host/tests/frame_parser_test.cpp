// frame_parser_test.cpp — Qt Test unit tests for FrameParser.
//
// Build: cmake -S .. -B build -DBUILD_TESTING=ON && cmake --build build
// Run:   ctest --test-dir build  (or run the executable directly)

#include <QtTest>
#include "frame_parser.h"

// ---- helpers ----------------------------------------------------------------

// Build a well-formed Device→Host frame.
static QByteArray d2h_frame(quint8 cmd, const QByteArray& payload)
{
    QByteArray f;
    f += static_cast<char>(0x55u); // ROBIN_MAGIC_D2H_H
    f += static_cast<char>(0xAAu); // ROBIN_MAGIC_D2H_L
    f += static_cast<char>(cmd);
    f += payload;
    f += static_cast<char>(0xAAu); // ROBIN_FOOTER_D2H_H
    f += static_cast<char>(0x55u); // ROBIN_FOOTER_D2H_L
    return f;
}

struct Frame {
    quint8     opcode;
    QByteArray payload;
};

// ---- test class -------------------------------------------------------------

class FrameParserTest : public QObject {
    Q_OBJECT

private slots:

    // ---- basic frame types --------------------------------------------------

    void ack_frame_parsed()
    {
        FrameParser p;
        QList<Frame> got;
        p.feed(d2h_frame(0xFEu, QByteArray(1, '\x01')),
               [&](quint8 op, const QByteArray& pay) {
                   got.push_back({op, pay});
               });

        QCOMPARE(got.size(), qsizetype(1));
        QCOMPARE(got[0].opcode,  quint8(0xFE));
        QCOMPARE(got[0].payload, QByteArray(1, '\x01'));
    }

    void nak_frame_parsed()
    {
        FrameParser p;
        QList<Frame> got;
        p.feed(d2h_frame(0xFFu, QByteArray(1, '\x03')),
               [&](quint8 op, const QByteArray& pay) {
                   got.push_back({op, pay});
               });

        QCOMPARE(got.size(), qsizetype(1));
        QCOMPARE(got[0].opcode,          quint8(0xFF));
        QCOMPARE(got[0].payload.at(0),   char(0x03));
    }

    void weather_frame_parsed()
    {
        FrameParser p;
        QList<Frame> got;
        const QByteArray payload(18, '\xAB');
        p.feed(d2h_frame(0x01u, payload),
               [&](quint8 op, const QByteArray& pay) {
                   got.push_back({op, pay});
               });

        QCOMPARE(got.size(), qsizetype(1));
        QCOMPARE(got[0].opcode,         quint8(0x01));
        QCOMPARE(got[0].payload.size(),  qsizetype(18));
        QCOMPARE(got[0].payload,         payload);
    }

    void status_frame_parsed()
    {
        FrameParser p;
        QList<Frame> got;
        p.feed(d2h_frame(0x05u, QByteArray(12, '\x01')),
               [&](quint8 op, const QByteArray& pay) {
                   got.push_back({op, pay});
               });

        QCOMPARE(got.size(), qsizetype(1));
        QCOMPARE(got[0].payload.size(), qsizetype(12));
    }

    void meta_frame_parsed()
    {
        FrameParser p;
        QList<Frame> got;
        p.feed(d2h_frame(0x02u, QByteArray(216, '\xCD')),
               [&](quint8 op, const QByteArray& pay) {
                   got.push_back({op, pay});
               });

        QCOMPARE(got.size(), qsizetype(1));
        QCOMPARE(got[0].payload.size(), qsizetype(216));
    }

    // ---- multi-frame --------------------------------------------------------

    void multiple_frames_back_to_back()
    {
        FrameParser p;
        int count = 0;
        auto h = [&](quint8, const QByteArray&) { ++count; };

        QByteArray data;
        data += d2h_frame(0xFEu, QByteArray(1, '\x06')); // ACK DB_FLUSH
        data += d2h_frame(0xFFu, QByteArray(1, '\x01')); // NAK
        data += d2h_frame(0x01u, QByteArray(18, '\x00')); // weather

        p.feed(data, h);
        QCOMPARE(count, 3);
    }

    // ---- resync / error recovery -------------------------------------------

    void garbage_preamble_then_valid_frame()
    {
        FrameParser p;
        QList<Frame> got;
        p.feed({}, [&](quint8, const QByteArray&) {}); // no-op warm-up

        QByteArray data(20, static_cast<char>(0xDEu)); // garbage
        data += d2h_frame(0xFEu, QByteArray(1, '\x07'));

        p.feed(data, [&](quint8 op, const QByteArray& pay) {
            got.push_back({op, pay});
        });

        QCOMPARE(got.size(), qsizetype(1));
        QCOMPARE(got[0].opcode, quint8(0xFE));
    }

    void unknown_opcode_skipped_valid_frame_follows()
    {
        FrameParser p;
        int count = 0;
        auto h = [&](quint8, const QByteArray&) { ++count; };

        // Frame with opcode 0x08 (unknown D→H opcode)
        QByteArray bad;
        bad += static_cast<char>(0x55u);
        bad += static_cast<char>(0xAAu);
        bad += static_cast<char>(0x08u);
        bad += static_cast<char>(0xAAu);
        bad += static_cast<char>(0x55u);

        bad += d2h_frame(0xFEu, QByteArray(1, '\x01'));

        p.feed(bad, h);
        QCOMPARE(count, 1); // only the valid ACK
    }

    void bad_footer_causes_resync()
    {
        FrameParser p;
        int count = 0;
        auto h = [&](quint8, const QByteArray&) { ++count; };

        QByteArray data;
        // ACK frame with corrupted footer byte (0xBB instead of 0xAA)
        data += static_cast<char>(0x55u);
        data += static_cast<char>(0xAAu);
        data += static_cast<char>(0xFEu); // ACK
        data += static_cast<char>(0x01u); // echoed cmd
        data += static_cast<char>(0xBBu); // bad footer H
        data += static_cast<char>(0x55u); // footer L (not reached)

        data += d2h_frame(0xFEu, QByteArray(1, '\x01')); // valid frame after

        p.feed(data, h);
        QCOMPARE(count, 1);
    }

    // ---- split feeds --------------------------------------------------------

    void frame_split_one_byte_at_a_time()
    {
        FrameParser p;
        QList<Frame> got;
        auto h = [&](quint8 op, const QByteArray& pay) {
            got.push_back({op, pay});
        };

        const QByteArray frame = d2h_frame(0xFEu, QByteArray(1, '\x05'));
        for (int i = 0; i < frame.size(); ++i) {
            p.feed(frame.mid(i, 1), h);
        }

        QCOMPARE(got.size(), qsizetype(1));
        QCOMPARE(got[0].opcode, quint8(0xFE));
    }

    void weather_frame_split_mid_payload()
    {
        FrameParser p;
        QList<Frame> got;
        const QByteArray payload(18, '\x7F');
        const QByteArray frame = d2h_frame(0x01u, payload);
        auto h = [&](quint8 op, const QByteArray& pay) {
            got.push_back({op, pay});
        };

        // Split: magic+cmd in first chunk, rest in second
        p.feed(frame.left(3),          h);
        p.feed(frame.mid(3),           h);

        QCOMPARE(got.size(), qsizetype(1));
        QCOMPARE(got[0].payload, payload);
    }

    // ---- payload content integrity -----------------------------------------

    void magic_bytes_inside_payload_not_treated_as_resync()
    {
        FrameParser p;
        QList<Frame> got;

        // Weather payload that contains 0x55 0xAA (the magic sequence)
        QByteArray payload(18, '\x00');
        payload[4] = static_cast<char>(0x55u);
        payload[5] = static_cast<char>(0xAAu);
        payload[6] = static_cast<char>(0x55u);
        payload[7] = static_cast<char>(0xAAu);

        p.feed(d2h_frame(0x01u, payload),
               [&](quint8 op, const QByteArray& pay) {
                   got.push_back({op, pay});
               });

        QCOMPARE(got.size(), qsizetype(1));
        QCOMPARE(got[0].payload, payload); // delivered byte-for-byte
    }

    // ---- reset() ------------------------------------------------------------

    void reset_discards_partial_frame()
    {
        FrameParser p;
        int count = 0;
        auto h = [&](quint8, const QByteArray&) { ++count; };

        const QByteArray frame = d2h_frame(0xFEu, QByteArray(1, '\x01'));
        // Feed magic + cmd only, then reset mid-frame
        p.feed(frame.left(3), h);
        p.reset();
        // Complete valid frame afterwards must parse cleanly
        p.feed(d2h_frame(0xFEu, QByteArray(1, '\x02')), h);

        QCOMPARE(count, 1);
    }
};

QTEST_APPLESS_MAIN(FrameParserTest)
#include "frame_parser_test.moc"
