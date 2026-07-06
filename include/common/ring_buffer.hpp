#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace me {

template <typename T, std::size_t Capacity>
class alignas(64) LockFreeSPSC {
    static_assert((Capacity & (Capacity - 1u)) == 0u, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "LockFreeSPSC requires trivially copyable payloads");

private:
    alignas(64) std::array<T, Capacity> buffer_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};

public:
    LockFreeSPSC() = default;
    LockFreeSPSC(const LockFreeSPSC&) = delete;
    LockFreeSPSC& operator=(const LockFreeSPSC&) = delete;

    [[nodiscard]] inline bool write(const T& data) noexcept {
        const std::size_t current_tail = tail_.load(std::memory_order_relaxed);
        const std::size_t current_head = head_.load(std::memory_order_acquire);
        if ((current_tail - current_head) == Capacity) [[unlikely]] {
            return false;
        }

        buffer_[current_tail & (Capacity - 1u)] = data;
        tail_.store(current_tail + 1u, std::memory_order_release);
        return true;
    }

    [[nodiscard]] inline bool read(T& data) noexcept {
        const std::size_t current_head = head_.load(std::memory_order_relaxed);
        const std::size_t current_tail = tail_.load(std::memory_order_acquire);
        if (current_head == current_tail) [[unlikely]] {
            return false;
        }

        data = buffer_[current_head & (Capacity - 1u)];
        head_.store(current_head + 1u, std::memory_order_release);
        return true;
    }

    [[nodiscard]] inline bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] inline std::size_t size() const noexcept {
        const std::size_t current_head = head_.load(std::memory_order_acquire);
        const std::size_t current_tail = tail_.load(std::memory_order_acquire);
        return current_tail - current_head;
    }

    [[nodiscard]] inline std::size_t capacity() const noexcept {
        return Capacity;
    }
};

}  // namespace me

