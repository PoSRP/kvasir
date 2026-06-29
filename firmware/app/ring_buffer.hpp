#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>

// SPSC lock-free ring buffer. Capacity is N-1 usable slots.
template<typename T, size_t N> class RingBuffer {
public:
    bool push(T val)
    {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) % N;
        if (next == tail_.load(std::memory_order_acquire))
            return false;
        buf_[head] = val;
        head_.store(next, std::memory_order_release);
        return true;
    }

    std::optional<T> pop()
    {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return std::nullopt;
        T val = buf_[tail];
        tail_.store((tail + 1) % N, std::memory_order_release);
        return val;
    }

    bool empty() const
    {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

private:
    std::array<T, N>    buf_{};
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};
