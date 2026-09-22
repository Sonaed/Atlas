#pragma once

#include <cstddef>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>

namespace creative::engine {

template <typename Key, typename Value>
class LruCache {
public:
    explicit LruCache(std::size_t capacity) : capacity_(capacity) {}

    std::optional<Value> get(const Key& key) {
        const auto it = entries_.find(key);
        if (it == entries_.end()) return std::nullopt;
        order_.splice(order_.begin(), order_, it->second.second);
        return it->second.first;
    }

    void put(Key key, Value value) {
        if (capacity_ == 0) return;
        const auto it = entries_.find(key);
        if (it != entries_.end()) {
            it->second.first = std::move(value);
            order_.splice(order_.begin(), order_, it->second.second);
            return;
        }
        order_.push_front(key);
        entries_.emplace(std::move(key), std::make_pair(std::move(value), order_.begin()));
        while (entries_.size() > capacity_) {
            entries_.erase(order_.back());
            order_.pop_back();
        }
    }

    bool contains(const Key& key) const { return entries_.find(key) != entries_.end(); }
    std::size_t size() const noexcept { return entries_.size(); }
    void clear() noexcept { entries_.clear(); order_.clear(); }

private:
    using Order = std::list<Key>;
    using Entry = std::pair<Value, typename Order::iterator>;
    std::size_t capacity_;
    Order order_;
    std::unordered_map<Key, Entry> entries_;
};

}
