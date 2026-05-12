// frame_parser.h — binary frame parser for the Robin Weather Station
// protocol. Wire format is defined in ../shared/protocol.h.
//
// SCAFFOLD: structure only. The actual framing logic, CRC validation, and
// state-machine reset behaviour will be implemented once ../shared/protocol.h
// is finalised. Pattern matches the firmware-side cdctask.c parser to keep
// host and device in lockstep.

#pragma once

#include <QByteArray>
#include <functional>

class FrameParser {
public:
    using FrameHandler = std::function<void(quint8 opcode,
                                            const QByteArray& payload)>;

    FrameParser();

    // Reset the state machine. Call after open() or after any framing error.
    void reset();

    // Feed received bytes; invokes the handler for each complete frame found.
    void feed(const QByteArray& bytes, const FrameHandler& on_frame);

private:
    // TODO: state machine fields per shared/protocol.h.
};
