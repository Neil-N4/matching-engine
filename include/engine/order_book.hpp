#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/config.hpp"
#include "common/memory_pool.hpp"
#include "common/types.hpp"

namespace me {

struct PriceLevel;

struct alignas(64) Order {
    OrderID id{0};
    Price price{0};
    Qty quantity{0};
    Side side{Side::Buy};
    Order* prev{nullptr};
    Order* next{nullptr};
    PriceLevel* level{nullptr};
};

struct alignas(64) PriceLevel {
    Price price{0};
    Qty total_volume{0};
    std::uint32_t order_count{0};
    Order* head{nullptr};
    Order* tail{nullptr};
};

struct TopOfBook {
    Price best_bid{0};
    Price best_ask{0};
    Qty best_bid_quantity{0};
    Qty best_ask_quantity{0};
};

struct ExecutionReport {
    Qty filled_quantity{0};
    std::uint64_t notional{0};
    Price last_price{0};
    BookStatus status{BookStatus::NotFound};
};

namespace detail {

enum class EntryState : std::uint8_t {
    Empty = 0,
    Occupied = 1,
    Tombstone = 2,
};

[[nodiscard]] inline std::uint64_t mix_u64(std::uint64_t value) noexcept {
    value ^= value >> 33u;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33u;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33u;
    return value;
}

template <std::size_t Capacity>
class alignas(64) OrderDirectory {
    static_assert((Capacity & (Capacity - 1u)) == 0u, "OrderDirectory capacity must be a power of two");

private:
    struct Entry {
        OrderID key{0};
        Order* value{nullptr};
        EntryState state{EntryState::Empty};
    };

    std::array<Entry, Capacity> table_{};
    std::size_t occupied_{0};

public:
    [[nodiscard]] inline Order* find(const OrderID id) noexcept {
        std::size_t idx = mix_u64(id) & (Capacity - 1u);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            Entry& entry = table_[idx];
            if (entry.state == EntryState::Occupied && entry.key == id) [[likely]] {
                return entry.value;
            }
            if (entry.state == EntryState::Empty) [[unlikely]] {
                return nullptr;
            }
            idx = (idx + 1u) & (Capacity - 1u);
        }
        return nullptr;
    }

    [[nodiscard]] inline const Order* find(const OrderID id) const noexcept {
        std::size_t idx = mix_u64(id) & (Capacity - 1u);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            const Entry& entry = table_[idx];
            if (entry.state == EntryState::Occupied && entry.key == id) [[likely]] {
                return entry.value;
            }
            if (entry.state == EntryState::Empty) [[unlikely]] {
                return nullptr;
            }
            idx = (idx + 1u) & (Capacity - 1u);
        }
        return nullptr;
    }

    [[nodiscard]] inline BookStatus insert(const OrderID id, Order* order) noexcept {
        if (occupied_ == Capacity) [[unlikely]] {
            return BookStatus::OrderDirectoryFull;
        }

        std::size_t idx = mix_u64(id) & (Capacity - 1u);
        std::size_t first_tombstone = Capacity;

        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            Entry& entry = table_[idx];
            if (entry.state == EntryState::Occupied && entry.key == id) [[unlikely]] {
                return BookStatus::Duplicate;
            }
            if (entry.state == EntryState::Tombstone && first_tombstone == Capacity) {
                first_tombstone = idx;
            }
            if (entry.state == EntryState::Empty) {
                const std::size_t target = first_tombstone == Capacity ? idx : first_tombstone;
                table_[target].key = id;
                table_[target].value = order;
                table_[target].state = EntryState::Occupied;
                ++occupied_;
                return BookStatus::Accepted;
            }
            idx = (idx + 1u) & (Capacity - 1u);
        }

        if (first_tombstone != Capacity) {
            table_[first_tombstone].key = id;
            table_[first_tombstone].value = order;
            table_[first_tombstone].state = EntryState::Occupied;
            ++occupied_;
            return BookStatus::Accepted;
        }

        return BookStatus::OrderDirectoryFull;
    }

    inline bool erase(const OrderID id) noexcept {
        std::size_t idx = mix_u64(id) & (Capacity - 1u);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            Entry& entry = table_[idx];
            if (entry.state == EntryState::Occupied && entry.key == id) [[likely]] {
                entry.value = nullptr;
                entry.state = EntryState::Tombstone;
                --occupied_;
                return true;
            }
            if (entry.state == EntryState::Empty) [[unlikely]] {
                return false;
            }
            idx = (idx + 1u) & (Capacity - 1u);
        }
        return false;
    }

    [[nodiscard]] inline std::size_t size() const noexcept {
        return occupied_;
    }
};

template <std::size_t Capacity>
class alignas(64) PriceLevelDirectory {
    static_assert((Capacity & (Capacity - 1u)) == 0u, "PriceLevelDirectory capacity must be a power of two");

private:
    struct Entry {
        Price key{0};
        PriceLevel level{};
        EntryState state{EntryState::Empty};
    };

    std::array<Entry, Capacity> table_{};
    std::size_t occupied_{0};

public:
    [[nodiscard]] inline PriceLevel* find(const Price price) noexcept {
        std::size_t idx = mix_u64(price) & (Capacity - 1u);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            Entry& entry = table_[idx];
            if (entry.state == EntryState::Occupied && entry.key == price) [[likely]] {
                return &entry.level;
            }
            if (entry.state == EntryState::Empty) [[unlikely]] {
                return nullptr;
            }
            idx = (idx + 1u) & (Capacity - 1u);
        }
        return nullptr;
    }

    [[nodiscard]] inline const PriceLevel* find(const Price price) const noexcept {
        std::size_t idx = mix_u64(price) & (Capacity - 1u);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            const Entry& entry = table_[idx];
            if (entry.state == EntryState::Occupied && entry.key == price) [[likely]] {
                return &entry.level;
            }
            if (entry.state == EntryState::Empty) [[unlikely]] {
                return nullptr;
            }
            idx = (idx + 1u) & (Capacity - 1u);
        }
        return nullptr;
    }

    [[nodiscard]] inline PriceLevel* get_or_create(const Price price, BookStatus& status) noexcept {
        std::size_t idx = mix_u64(price) & (Capacity - 1u);
        std::size_t first_tombstone = Capacity;

        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            Entry& entry = table_[idx];
            if (entry.state == EntryState::Occupied && entry.key == price) [[likely]] {
                status = BookStatus::Accepted;
                return &entry.level;
            }
            if (entry.state == EntryState::Tombstone && first_tombstone == Capacity) {
                first_tombstone = idx;
            }
            if (entry.state == EntryState::Empty) {
                if (occupied_ == Capacity) [[unlikely]] {
                    status = BookStatus::PriceLevelFull;
                    return nullptr;
                }
                const std::size_t target = first_tombstone == Capacity ? idx : first_tombstone;
                Entry& target_entry = table_[target];
                target_entry.key = price;
                target_entry.level = PriceLevel{price, 0u, 0u, nullptr, nullptr};
                target_entry.state = EntryState::Occupied;
                ++occupied_;
                status = BookStatus::Accepted;
                return &target_entry.level;
            }
            idx = (idx + 1u) & (Capacity - 1u);
        }

        if (first_tombstone != Capacity && occupied_ < Capacity) {
            Entry& entry = table_[first_tombstone];
            entry.key = price;
            entry.level = PriceLevel{price, 0u, 0u, nullptr, nullptr};
            entry.state = EntryState::Occupied;
            ++occupied_;
            status = BookStatus::Accepted;
            return &entry.level;
        }

        status = BookStatus::PriceLevelFull;
        return nullptr;
    }

    inline bool erase_if_empty(const Price price) noexcept {
        std::size_t idx = mix_u64(price) & (Capacity - 1u);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            Entry& entry = table_[idx];
            if (entry.state == EntryState::Occupied && entry.key == price) [[likely]] {
                if (entry.level.order_count != 0u) {
                    return false;
                }
                entry.level = {};
                entry.state = EntryState::Tombstone;
                --occupied_;
                return true;
            }
            if (entry.state == EntryState::Empty) [[unlikely]] {
                return false;
            }
            idx = (idx + 1u) & (Capacity - 1u);
        }
        return false;
    }

    template <typename Fn>
    inline void for_each(Fn&& fn) const noexcept {
        for (const Entry& entry : table_) {
            if (entry.state == EntryState::Occupied && entry.level.order_count > 0u) {
                fn(entry.level);
            }
        }
    }

    [[nodiscard]] inline std::size_t size() const noexcept {
        return occupied_;
    }
};

}  // namespace detail

template <std::size_t MaxOrders = config::kDefaultMaxOrders,
          std::size_t OrderDirectoryCapacity = config::kDefaultOrderDirectoryCapacity,
          std::size_t PriceLevelCapacity = config::kDefaultPriceLevelCapacity>
class alignas(64) OrderBook {
private:
    FixedSlabPool<Order, MaxOrders> order_pool_{};
    detail::OrderDirectory<OrderDirectoryCapacity> orders_{};
    detail::PriceLevelDirectory<PriceLevelCapacity> bid_levels_{};
    detail::PriceLevelDirectory<PriceLevelCapacity> ask_levels_{};
    Price best_bid_{0};
    Price best_ask_{0};
    Qty total_bid_volume_{0};
    Qty total_ask_volume_{0};
    std::uint64_t executed_notional_{0};
    Qty executed_quantity_{0};

public:
    OrderBook() = default;
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    [[nodiscard]] inline BookStatus add_order(const OrderID id,
                                              const Side side,
                                              const Price price,
                                              const Qty quantity) noexcept {
        if (quantity == 0u) [[unlikely]] {
            return BookStatus::InvalidQuantity;
        }
        if (orders_.find(id) != nullptr) [[unlikely]] {
            return BookStatus::Duplicate;
        }

        BookStatus level_status = BookStatus::Accepted;
        PriceLevel* const level = levels(side).get_or_create(price, level_status);
        if (level == nullptr) [[unlikely]] {
            return level_status;
        }

        Order* const order = order_pool_.allocate();
        if (order == nullptr) [[unlikely]] {
            levels(side).erase_if_empty(price);
            return BookStatus::OrderPoolExhausted;
        }

        *order = Order{id, price, quantity, side, nullptr, nullptr, level};
        const BookStatus directory_status = orders_.insert(id, order);
        if (directory_status != BookStatus::Accepted) [[unlikely]] {
            order_pool_.deallocate(order);
            levels(side).erase_if_empty(price);
            return directory_status;
        }

        append_order(*level, *order);
        add_side_volume(side, quantity);
        update_best_on_add(side, price);
        return BookStatus::Accepted;
    }

    [[nodiscard]] inline BookStatus cancel_order(const OrderID id, const Qty quantity) noexcept {
        Order* const order = orders_.find(id);
        if (order == nullptr) [[unlikely]] {
            return BookStatus::NotFound;
        }
        if (quantity == 0u) [[unlikely]] {
            return BookStatus::InvalidQuantity;
        }

        const Qty removed = quantity >= order->quantity ? order->quantity : quantity;
        order->quantity -= removed;
        order->level->total_volume -= removed;
        subtract_side_volume(order->side, removed);

        if (order->quantity == 0u) {
            remove_order(order);
            return BookStatus::Filled;
        }
        return BookStatus::Partial;
    }

    [[nodiscard]] inline BookStatus execute_order(const OrderID id, const Qty quantity) noexcept {
        Order* const order = orders_.find(id);
        if (order == nullptr) [[unlikely]] {
            return BookStatus::NotFound;
        }
        if (quantity == 0u) [[unlikely]] {
            return BookStatus::InvalidQuantity;
        }

        const Qty executed = quantity >= order->quantity ? order->quantity : quantity;
        const Price price = order->price;
        const Side side = order->side;
        order->quantity -= executed;
        order->level->total_volume -= executed;
        subtract_side_volume(side, executed);
        executed_notional_ += static_cast<std::uint64_t>(price) * executed;
        executed_quantity_ += executed;

        if (order->quantity == 0u) {
            remove_order(order);
            return BookStatus::Filled;
        }
        return BookStatus::Partial;
    }

    [[nodiscard]] inline ExecutionReport match_market_order(const Side aggressor_side,
                                                            Qty quantity) noexcept {
        ExecutionReport report{};
        if (quantity == 0u) [[unlikely]] {
            report.status = BookStatus::InvalidQuantity;
            return report;
        }

        const Side resting_side = opposite(aggressor_side);
        while (quantity > 0u) {
            PriceLevel* const level = best_level(resting_side);
            if (level == nullptr || level->head == nullptr) [[unlikely]] {
                break;
            }

            Order* const resting = level->head;
            const Qty fill = quantity < resting->quantity ? quantity : resting->quantity;
            const Price price = resting->price;

            resting->quantity -= fill;
            level->total_volume -= fill;
            subtract_side_volume(resting_side, fill);
            quantity -= fill;

            report.filled_quantity += fill;
            report.notional += static_cast<std::uint64_t>(price) * fill;
            report.last_price = price;
            executed_notional_ += static_cast<std::uint64_t>(price) * fill;
            executed_quantity_ += fill;

            if (resting->quantity == 0u) {
                remove_order(resting);
            }
        }

        if (report.filled_quantity == 0u) {
            report.status = BookStatus::NotFound;
        } else if (quantity == 0u) {
            report.status = BookStatus::Filled;
        } else {
            report.status = BookStatus::Partial;
        }
        return report;
    }

    [[nodiscard]] inline ExecutionReport submit_limit_order(const OrderID id,
                                                            const Side side,
                                                            const Price price,
                                                            Qty quantity) noexcept {
        ExecutionReport report{};
        if (quantity == 0u) [[unlikely]] {
            report.status = BookStatus::InvalidQuantity;
            return report;
        }

        while (quantity > 0u && crosses(side, price)) {
            PriceLevel* const level = best_level(opposite(side));
            if (level == nullptr || level->head == nullptr) [[unlikely]] {
                break;
            }
            Order* const resting = level->head;
            const Qty fill = quantity < resting->quantity ? quantity : resting->quantity;
            const Price fill_price = resting->price;

            resting->quantity -= fill;
            level->total_volume -= fill;
            subtract_side_volume(resting->side, fill);
            quantity -= fill;

            report.filled_quantity += fill;
            report.notional += static_cast<std::uint64_t>(fill_price) * fill;
            report.last_price = fill_price;
            executed_notional_ += static_cast<std::uint64_t>(fill_price) * fill;
            executed_quantity_ += fill;

            if (resting->quantity == 0u) {
                remove_order(resting);
            }
        }

        if (quantity > 0u) {
            const BookStatus add_status = add_order(id, side, price, quantity);
            if (report.filled_quantity == 0u) {
                report.status = add_status;
            } else {
                report.status = add_status == BookStatus::Accepted ? BookStatus::Partial : add_status;
            }
        } else {
            report.status = BookStatus::Filled;
        }

        return report;
    }

    [[nodiscard]] inline const Order* find_order(const OrderID id) const noexcept {
        return orders_.find(id);
    }

    [[nodiscard]] inline TopOfBook top_of_book() const noexcept {
        const PriceLevel* const bid = best_bid_ == 0u ? nullptr : bid_levels_.find(best_bid_);
        const PriceLevel* const ask = best_ask_ == 0u ? nullptr : ask_levels_.find(best_ask_);
        return TopOfBook{
            best_bid_,
            best_ask_,
            bid == nullptr ? 0u : bid->total_volume,
            ask == nullptr ? 0u : ask->total_volume,
        };
    }

    [[nodiscard]] inline Qty total_bid_volume() const noexcept {
        return total_bid_volume_;
    }

    [[nodiscard]] inline Qty total_ask_volume() const noexcept {
        return total_ask_volume_;
    }

    [[nodiscard]] inline Qty executed_quantity() const noexcept {
        return executed_quantity_;
    }

    [[nodiscard]] inline std::uint64_t executed_notional() const noexcept {
        return executed_notional_;
    }

    [[nodiscard]] inline std::size_t live_orders() const noexcept {
        return orders_.size();
    }

    [[nodiscard]] inline std::size_t free_order_slots() const noexcept {
        return order_pool_.available();
    }

private:
    [[nodiscard]] inline detail::PriceLevelDirectory<PriceLevelCapacity>& levels(const Side side) noexcept {
        return side == Side::Buy ? bid_levels_ : ask_levels_;
    }

    [[nodiscard]] inline const detail::PriceLevelDirectory<PriceLevelCapacity>& levels(const Side side) const noexcept {
        return side == Side::Buy ? bid_levels_ : ask_levels_;
    }

    inline void append_order(PriceLevel& level, Order& order) noexcept {
        order.prev = level.tail;
        order.next = nullptr;
        if (level.tail != nullptr) {
            level.tail->next = &order;
        } else {
            level.head = &order;
        }
        level.tail = &order;
        level.total_volume += order.quantity;
        ++level.order_count;
    }

    inline void unlink_order(Order& order) noexcept {
        PriceLevel& level = *order.level;
        if (order.prev != nullptr) {
            order.prev->next = order.next;
        } else {
            level.head = order.next;
        }

        if (order.next != nullptr) {
            order.next->prev = order.prev;
        } else {
            level.tail = order.prev;
        }

        --level.order_count;
        order.prev = nullptr;
        order.next = nullptr;
    }

    inline void remove_order(Order* order) noexcept {
        const Side side = order->side;
        const Price price = order->price;
        unlink_order(*order);
        orders_.erase(order->id);

        PriceLevel* const level = levels(side).find(price);
        if (level != nullptr && level->order_count == 0u) {
            levels(side).erase_if_empty(price);
            update_best_after_level_removed(side, price);
        }

        order_pool_.deallocate(order);
    }

    inline void add_side_volume(const Side side, const Qty quantity) noexcept {
        if (side == Side::Buy) {
            total_bid_volume_ += quantity;
        } else {
            total_ask_volume_ += quantity;
        }
    }

    inline void subtract_side_volume(const Side side, const Qty quantity) noexcept {
        if (side == Side::Buy) {
            total_bid_volume_ -= quantity;
        } else {
            total_ask_volume_ -= quantity;
        }
    }

    inline void update_best_on_add(const Side side, const Price price) noexcept {
        if (side == Side::Buy) {
            if (price > best_bid_) {
                best_bid_ = price;
            }
        } else if (best_ask_ == 0u || price < best_ask_) {
            best_ask_ = price;
        }
    }

    inline void update_best_after_level_removed(const Side side, const Price removed_price) noexcept {
        if (side == Side::Buy && removed_price == best_bid_) {
            best_bid_ = find_best_bid();
        } else if (side == Side::Sell && removed_price == best_ask_) {
            best_ask_ = find_best_ask();
        }
    }

    [[nodiscard]] inline Price find_best_bid() const noexcept {
        Price best = 0u;
        bid_levels_.for_each([&best](const PriceLevel& level) noexcept {
            if (level.price > best) {
                best = level.price;
            }
        });
        return best;
    }

    [[nodiscard]] inline Price find_best_ask() const noexcept {
        Price best = 0u;
        ask_levels_.for_each([&best](const PriceLevel& level) noexcept {
            if (best == 0u || level.price < best) {
                best = level.price;
            }
        });
        return best;
    }

    [[nodiscard]] inline PriceLevel* best_level(const Side side) noexcept {
        const Price price = side == Side::Buy ? best_bid_ : best_ask_;
        if (price == 0u) {
            return nullptr;
        }
        return levels(side).find(price);
    }

    [[nodiscard]] inline bool crosses(const Side side, const Price price) const noexcept {
        if (side == Side::Buy) {
            return best_ask_ != 0u && price >= best_ask_;
        }
        return best_bid_ != 0u && price <= best_bid_;
    }
};

}  // namespace me

