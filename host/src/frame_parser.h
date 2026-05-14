// frame_parser.h — Robin Weather Station binary frame parser (Device→Host).
// Wire format defined in ../shared/protocol.h.
//
// Byte-at-a-time state machine. Payload length is looked up from the opcode
// table (no length field on the wire). Resyncs on unknown opcode or footer
// mismatch — USB CDC provides transport integrity, so these are logic-error
// guards only.

#pragma once

#include <QByteArray>
#include <functional>

class FrameParser {
public:
    using FrameHandler = std::function<void(quint8 opcode,
                                            const QByteArray& payload)>;

    FrameParser();

    // Reset the state machine. Call after port open or on framing error.
    void reset();

    // Feed received bytes; calls on_frame once per complete valid frame.
    void feed(const QByteArray& bytes, const FrameHandler& on_frame);

private:
    enum class State : quint8 {
        WaitMagicH,
        WaitMagicL,
        WaitCmd,
        RecvPayload,
        WaitFooterH,
        WaitFooterL,
    };

    // Expected payload byte count for a D→H opcode; -1 if opcode is unknown.
    static int payloadLen(quint8 opcode);

    State      m_state;
    quint8     m_cmd;
    int        m_payloadExpected;
    QByteArray m_payload;
};
