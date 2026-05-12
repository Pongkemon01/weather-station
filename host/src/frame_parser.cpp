// frame_parser.cpp — binary frame parser (scaffold).

#include "frame_parser.h"

FrameParser::FrameParser() {
    reset();
}

void FrameParser::reset() {
    // TODO: clear state machine.
}

void FrameParser::feed(const QByteArray& bytes, const FrameHandler& on_frame) {
    Q_UNUSED(bytes);
    Q_UNUSED(on_frame);
    // TODO: byte-at-a-time state machine; invoke on_frame for each completed
    // frame with valid CRC. Reset on framing error.
}
