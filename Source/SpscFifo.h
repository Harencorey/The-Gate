#pragma once
//==============================================================================
// Single-producer / single-consumer lock-free FIFO with a fixed capacity.
// The audio thread pushes (never blocks, never allocates; drops when full),
// the UI thread pops. Capacity must be a power of two.
//==============================================================================

#include <array>
#include <atomic>
#include <cstddef>

namespace tg
{

template <typename T, size_t Capacity>
class SpscFifo
{
    static_assert ((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    bool push (const T& item) noexcept
    {
        const size_t w = write.load (std::memory_order_relaxed);
        if (w - read.load (std::memory_order_acquire) >= Capacity)
            return false;   // full: drop rather than block the audio thread
        items[w & (Capacity - 1)] = item;
        write.store (w + 1, std::memory_order_release);
        return true;
    }

    bool pop (T& item) noexcept
    {
        const size_t r = read.load (std::memory_order_relaxed);
        if (r == write.load (std::memory_order_acquire))
            return false;
        item = items[r & (Capacity - 1)];
        read.store (r + 1, std::memory_order_release);
        return true;
    }

    void clear() noexcept { read.store (write.load (std::memory_order_acquire), std::memory_order_release); }

private:
    std::array<T, Capacity> items {};
    std::atomic<size_t> write { 0 }, read { 0 };
};

} // namespace tg
