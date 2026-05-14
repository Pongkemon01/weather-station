// frame_parser.cpp — Robin Weather Station binary frame parser.

#include "frame_parser.h"
#include "protocol.h"

FrameParser::FrameParser() {
    reset();
}

void FrameParser::reset() {
    m_state           = State::WaitMagicH;
    m_cmd             = 0;
    m_payloadExpected = 0;
    m_payload.clear();
}

// Returns expected payload bytes for each Device→Host opcode.
// Opcodes 0x01–0x07 are host-originated commands; the device only ever sends
// back 0x01 (weather), 0x02 (meta), 0x05 (status), 0xFE (ACK), or 0xFF (NAK).
int FrameParser::payloadLen(quint8 opcode) {
    switch (opcode) {
    case ROBIN_OP_REQ_WEATHER:  return static_cast<int>(sizeof(Weather_Data_Packed_t));   // 18
    case ROBIN_OP_REQ_META:     return static_cast<int>(sizeof(Meta_Data_t));             // 216
    case ROBIN_OP_REQ_STATUS:   return static_cast<int>(sizeof(System_Ready_Status_t));   // 12
    case ROBIN_OP_ACK:          return 1;   // echoed command byte
    case ROBIN_OP_NAK:          return 1;   // error code byte
    default:                    return -1;
    }
}

void FrameParser::feed(const QByteArray& bytes, const FrameHandler& on_frame) {
    for (const char ch : bytes) {
        const quint8 b = static_cast<quint8>(ch);

        switch (m_state) {

        case State::WaitMagicH:
            if (b == ROBIN_MAGIC_D2H_H) {
                m_state = State::WaitMagicL;
            }
            break;

        case State::WaitMagicL:
            if (b == ROBIN_MAGIC_D2H_L) {
                m_state = State::WaitCmd;
            } else if (b == ROBIN_MAGIC_D2H_H) {
                // consecutive 0x55 — first byte of a new magic pair, stay
            } else {
                m_state = State::WaitMagicH;
            }
            break;

        case State::WaitCmd: {
            const int len = payloadLen(b);
            if (len < 0) {
                m_state = State::WaitMagicH;   // unknown opcode — drop and resync
                break;
            }
            m_cmd             = b;
            m_payloadExpected = len;
            m_payload.clear();
            m_payload.reserve(static_cast<qsizetype>(len));
            m_state = (len > 0) ? State::RecvPayload : State::WaitFooterH;
            break;
        }

        case State::RecvPayload:
            m_payload.append(static_cast<char>(b));
            if (m_payload.size() == static_cast<qsizetype>(m_payloadExpected)) {
                m_state = State::WaitFooterH;
            }
            break;

        case State::WaitFooterH:
            if (b == ROBIN_FOOTER_D2H_H) {
                m_state = State::WaitFooterL;
            } else if (b == ROBIN_MAGIC_D2H_H) {
                m_state = State::WaitMagicL;   // new frame starting — resync
            } else {
                m_state = State::WaitMagicH;
            }
            break;

        case State::WaitFooterL:
            if (b == ROBIN_FOOTER_D2H_L) {
                on_frame(m_cmd, m_payload);
                m_state = State::WaitMagicH;
            } else if (b == ROBIN_MAGIC_D2H_H) {
                m_state = State::WaitMagicL;
            } else {
                m_state = State::WaitMagicH;
            }
            break;
        }
    }
}
