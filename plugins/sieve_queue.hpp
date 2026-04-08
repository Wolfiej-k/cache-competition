#pragma once

#include <cstdint>
#include <list>
#include <optional>
#include <unordered_map>
#include <cassert>

template <typename T> class SieveQueue {
  private:
    struct ObjectMeta {
        uint64_t size;
        bool visited;
        typename std::list<T>::iterator it;
    };

    std::list<T> queue_;
    std::unordered_map<T, ObjectMeta> meta_;

    typename std::list<T>::iterator hand_;
    uint64_t current_size_bytes_;

    void advance_hand() {
        if (hand_ == queue_.begin()) {
            hand_ = std::prev(queue_.end());
        } else {
            --hand_;
        }
    }

  public:
    SieveQueue() : current_size_bytes_(0) { hand_ = queue_.end(); }

    uint64_t get_size_bytes() const { return current_size_bytes_; }
    bool contains(T id) const { return meta_.count(id) > 0; }

    void push(T id, uint64_t size) {
        queue_.push_front(id);
        meta_[id] = {size, false, queue_.begin()};
        current_size_bytes_ += size;

        if (queue_.size() == 1) {
            hand_ = queue_.begin();
        }
    }

    void mark_visited(T id) {
        auto it = meta_.find(id);
        if (it != meta_.end()) {
            it->second.visited = true;
        }
    }

    std::optional<T> evict() {
        if (queue_.empty()) {
            return {};
        }

        assert(hand_ != queue_.end());

        while (true) {
            T curr_id = *hand_;
            auto meta_it = meta_.find(curr_id);
            assert(meta_it != meta_.end());

            if (meta_it->second.visited) {
                meta_it->second.visited = false;
                advance_hand();
            } else {
                T victim = curr_id;
                uint64_t v_size = meta_it->second.size;
                auto erase_it = hand_;

                if (queue_.size() == 1) {
                    hand_ = queue_.end();
                } else {
                    advance_hand();
                }

                queue_.erase(erase_it);
                meta_.erase(victim);
                current_size_bytes_ -= v_size;

                return victim;
            }
        }
    }

    void remove(T id) {
        auto it = meta_.find(id);
        if (it != meta_.end()) {
            if (hand_ == it->second.it) {
                if (queue_.size() == 1) {
                    hand_ = queue_.end();
                } else {
                    advance_hand();
                }
            }

            current_size_bytes_ -= it->second.size;
            queue_.erase(it->second.it);
            meta_.erase(it);
        }
    }
};
