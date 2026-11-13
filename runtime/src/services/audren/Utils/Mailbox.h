// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Utils/Mailbox.cs: a message queue for unmanaged types with a message and a response channel.
// C# BlockingCollection<T>(1) is replaced by a bounded queue (capacity 1) built on std::mutex and std::condition_variable:
// Add blocks while the queue is full, Take blocks while it is empty.
#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <type_traits>

#include "../audren_common.h"

namespace nxrt::audren {

template <class T>
class Mailbox {
    static_assert(std::is_trivially_copyable_v<T>, "unmanaged");

public:
    Mailbox() = default;
    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;

    void SendMessage(T data) { _messageQueue.Add(data); }

    void SendResponse(T data) { _responseQueue.Add(data); }

    T ReceiveMessage() { return _messageQueue.Take(); }

    T ReceiveResponse() { return _responseQueue.Take(); }

    // C# IDisposable.Dispose disposes the two BlockingCollections. The C++ mutexes and condition variables are released by the
    // destructor, so this is a no-op kept for the same call shape.
    void Dispose() {}

private:
    // A bounded blocking FIFO (C# BlockingCollection<T>(capacity)).
    class BoundedQueue {
    public:
        explicit BoundedQueue(size_t capacity) : _capacity(capacity) {}

        void Add(T data) {
            std::unique_lock<std::mutex> lock(_mutex);
            _notFull.wait(lock, [this] { return _items.size() < _capacity; });
            _items.push_back(data);
            _notEmpty.notify_one();
        }

        T Take() {
            std::unique_lock<std::mutex> lock(_mutex);
            _notEmpty.wait(lock, [this] { return !_items.empty(); });
            T data = _items.front();
            _items.pop_front();
            _notFull.notify_one();
            return data;
        }

    private:
        std::mutex _mutex;
        std::condition_variable _notFull;
        std::condition_variable _notEmpty;
        std::deque<T> _items;
        size_t _capacity;
    };

    BoundedQueue _messageQueue{1};
    BoundedQueue _responseQueue{1};
};

}  // namespace nxrt::audren
