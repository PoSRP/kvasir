#ifndef KVASIR_RING_BUFFER_HPP
#define KVASIR_RING_BUFFER_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>

namespace kvasir {

// SPSC lock-free FIFO
template<typename T, size_t N> class RingBuffer {
public:
    bool push(T val)
    {
        size_t head = _head.load(std::memory_order_relaxed);
        size_t next = (head + 1) % N;
        if (next == _tail.load(std::memory_order_acquire)) {
            return false;
        }
        _buf[head] = val;
        _head.store(next, std::memory_order_release);
        return true;
    }

    std::optional<T> pop()
    {
        size_t tail = _tail.load(std::memory_order_relaxed);
        if (tail == _head.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        T val = _buf[tail];
        _tail.store((tail + 1) % N, std::memory_order_release);
        return val;
    }

    [[nodiscard]] bool empty() const
    {
        return _tail.load(std::memory_order_acquire) == _head.load(std::memory_order_acquire);
    }

private:
    std::array<T, N>    _buf{};
    std::atomic<size_t> _head{0};
    std::atomic<size_t> _tail{0};
};

} // namespace kvasir

#endif // KVASIR_RING_BUFFER_HPP
