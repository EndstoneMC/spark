#ifndef ENDSTONE_SPARK_BOUNDED_EVENT_QUEUE_H
#define ENDSTONE_SPARK_BOUNDED_EVENT_QUEUE_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace spark {

// Fixed-capacity MPMC ring used where enqueueing must never allocate. Capacity
// must be a power of two so positions can wrap without division.
template <typename Event, std::size_t Capacity>
class BoundedEventQueue {
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);

    struct Cell {
        std::atomic<std::size_t> sequence{0};
        Event event{};
    };

public:
    BoundedEventQueue() noexcept
    {
        for (std::size_t i = 0; i < Capacity; ++i) {
            storage_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    bool enqueue(const Event &event) noexcept
    {
        std::size_t position = producer_.load(std::memory_order_relaxed);
        Cell *cell = nullptr;
        for (;;) {
            cell = &storage_[position & (Capacity - 1)];
            const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
            const std::intptr_t difference =
                static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position);
            if (difference == 0) {
                if (producer_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                    break;
                }
            }
            else if (difference < 0) {
                return false;
            }
            else {
                position = producer_.load(std::memory_order_relaxed);
            }
        }
        cell->event = event;
        cell->sequence.store(position + 1, std::memory_order_release);
        return true;
    }

    bool dequeue(Event &event) noexcept
    {
        std::size_t position = consumer_.load(std::memory_order_relaxed);
        Cell *cell = nullptr;
        for (;;) {
            cell = &storage_[position & (Capacity - 1)];
            const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
            const std::intptr_t difference =
                static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position + 1);
            if (difference == 0) {
                if (consumer_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                    break;
                }
            }
            else if (difference < 0) {
                return false;
            }
            else {
                position = consumer_.load(std::memory_order_relaxed);
            }
        }
        event = cell->event;
        cell->sequence.store(position + Capacity, std::memory_order_release);
        return true;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    std::array<Cell, Capacity> storage_{};
    alignas(64) std::atomic<std::size_t> producer_{0};
    alignas(64) std::atomic<std::size_t> consumer_{0};
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_BOUNDED_EVENT_QUEUE_H
