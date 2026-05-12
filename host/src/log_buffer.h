// log_buffer.h — in-memory ring buffer for the session log viewer.
//
// IMPORTANT: This buffer is volatile by design. Per the project spec, the
// host MUST NOT persist log entries to disk. Entries are kept only for the
// duration of the application's run.

#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QMutex>
#include <deque>

class LogBuffer : public QObject {
    Q_OBJECT

public:
    enum class Level { Debug, Info, Warning, Error };
    Q_ENUM(Level)

    struct Entry {
        QDateTime when;
        Level     level;
        QString   message;
    };

    explicit LogBuffer(QObject* parent = nullptr, int capacity = 2000);

    void append(Level level, const QString& msg);
    std::deque<Entry> snapshot() const;
    void clear();

    // Convenience helpers.
    void debug   (const QString& msg) { append(Level::Debug,   msg); }
    void info    (const QString& msg) { append(Level::Info,    msg); }
    void warning (const QString& msg) { append(Level::Warning, msg); }
    void error   (const QString& msg) { append(Level::Error,   msg); }

signals:
    void entryAdded(const LogBuffer::Entry& entry);
    void cleared();

private:
    mutable QMutex    mutex_;
    std::deque<Entry> entries_;
    int               capacity_;
};
