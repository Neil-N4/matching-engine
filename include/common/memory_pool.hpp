#pragma once

#include <array>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace me {

template <typename T, std::size_t BlockCount>
class alignas(64) FixedSlabPool {
    static_assert(BlockCount > 0, "FixedSlabPool requires at least one block");

private:
    struct alignas(64) Chunk {
        alignas(T) std::byte storage[sizeof(T)];
        Chunk* next{nullptr};
    };

    std::array<Chunk, BlockCount> storage_{};
    Chunk* free_list_{nullptr};
    std::size_t available_{BlockCount};

public:
    FixedSlabPool() noexcept {
        for (std::size_t i = 0; i + 1 < BlockCount; ++i) {
            storage_[i].next = &storage_[i + 1];
        }
        storage_[BlockCount - 1].next = nullptr;
        free_list_ = &storage_[0];
    }

    FixedSlabPool(const FixedSlabPool&) = delete;
    FixedSlabPool& operator=(const FixedSlabPool&) = delete;

    template <typename... Args>
    [[nodiscard]] inline T* allocate(Args&&... args) noexcept {
        if (free_list_ == nullptr) [[unlikely]] {
            return nullptr;
        }

        Chunk* const chunk = free_list_;
        free_list_ = free_list_->next;
        --available_;
        return ::new (static_cast<void*>(chunk->storage)) T(std::forward<Args>(args)...);
    }

    inline void deallocate(T* ptr) noexcept {
        if (ptr == nullptr) [[unlikely]] {
            return;
        }

        ptr->~T();
        Chunk* const chunk = reinterpret_cast<Chunk*>(ptr);
        chunk->next = free_list_;
        free_list_ = chunk;
        ++available_;
    }

    [[nodiscard]] inline std::size_t capacity() const noexcept {
        return BlockCount;
    }

    [[nodiscard]] inline std::size_t available() const noexcept {
        return available_;
    }

    [[nodiscard]] inline std::size_t used() const noexcept {
        return BlockCount - available_;
    }
};

}  // namespace me

