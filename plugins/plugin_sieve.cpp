#include <cassert>
#include <cstdint>
#include <libCacheSim.h>
#include <list>
#include <optional>
#include <unordered_map>

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

class SieveCache {
  private:
    SieveQueue<obj_id_t> sieve_;
    uint64_t cache_capacity_;

  public:
    SieveCache(uint64_t capacity) : cache_capacity_(capacity) {}

    void on_hit(obj_id_t id) { sieve_.mark_visited(id); }

    void on_miss(obj_id_t id, uint64_t size) {
        if (size > cache_capacity_) {
            return;
        }
        sieve_.push(id, size);
    }

    obj_id_t evict() {
        auto victim = sieve_.evict();
        assert(victim.has_value());
        return *victim;
    }

    void on_remove(obj_id_t id) { sieve_.remove(id); }
};

extern "C" {
void *cache_init_hook(const common_cache_params_t params) {
    return new SieveCache(params.cache_size);
}

void cache_hit_hook(void *data, const request_t *req) {
    static_cast<SieveCache *>(data)->on_hit(req->obj_id);
}

void cache_miss_hook(void *data, const request_t *req) {
    static_cast<SieveCache *>(data)->on_miss(req->obj_id, req->obj_size);
}

obj_id_t cache_eviction_hook(void *data, const request_t * /*req*/) {
    return static_cast<SieveCache *>(data)->evict();
}

void cache_remove_hook(void *data, obj_id_t obj_id) {
    static_cast<SieveCache *>(data)->on_remove(obj_id);
}

void cache_free_hook(void *data) {
    SieveCache *fifo_cache = (SieveCache *)data;
    delete fifo_cache;
}
} // extern "C"
