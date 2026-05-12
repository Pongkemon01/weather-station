// log_buffer.cpp — in-memory ring buffer implementation.

#include "log_buffer.h"

#include <QMutexLocker>

LogBuffer::LogBuffer(QObject* parent, int capacity)
    : QObject(parent), capacity_(capacity > 0 ? capacity : 1) {
}

void LogBuffer::append(Level level, const QString& msg) {
    Entry entry { QDateTime::currentDateTime(), level, msg };
    {
        QMutexLocker locker(&mutex_);
        entries_.push_back(entry);
        while (static_cast<int>(entries_.size()) > capacity_) {
            entries_.pop_front();
        }
    }
    emit entryAdded(entry);
}

std::deque<LogBuffer::Entry> LogBuffer::snapshot() const {
    QMutexLocker locker(&mutex_);
    return entries_;
}

void LogBuffer::clear() {
    {
        QMutexLocker locker(&mutex_);
        entries_.clear();
    }
    emit cleared();
}
