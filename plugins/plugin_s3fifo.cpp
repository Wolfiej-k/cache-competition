#include <algorithm>
#include <libCacheSim.h>
#include <list>
#include <optional>
#include <unordered_map>

class S3FifoCache {
  private:
    enum QueueType : uint8_t { NONE, SMALL, MAIN, GHOST };

    struct ObjectMeta {
        uint64_t size;
        uint8_t freq;
        QueueType q_type;
        std::list<obj_id_t>::iterator it;
    };

    std::list<obj_id_t> small_;
    std::list<obj_id_t> main_;
    std::list<obj_id_t> ghost_;

    std::unordered_map<obj_id_t, ObjectMeta> meta_;

    uint64_t cache_size_;
    uint64_t s_size_bytes_;
    uint64_t m_size_bytes_;
    uint64_t g_size_bytes_;

  public:
    S3FifoCache(uint64_t cache_size)
        : cache_size_(cache_size), s_size_bytes_(0), m_size_bytes_(0),
          g_size_bytes_(0) {}

    bool can_fit(uint64_t size) const noexcept {
        return s_size_bytes_ + m_size_bytes_ + size <= cache_size_;
    }

    void push_main(obj_id_t id, uint64_t size, uint8_t freq) {
        main_.push_front(id);
        meta_[id] = {size, freq, MAIN, main_.begin()};
        m_size_bytes_ += size;
    }

    void push_small(obj_id_t id, uint64_t size, uint8_t freq) {
        small_.push_front(id);
        meta_[id] = {size, freq, SMALL, small_.begin()};
        s_size_bytes_ += size;
    }

    void push_ghost(obj_id_t id, uint64_t size, uint8_t freq) {
        ghost_.push_front(id);
        meta_[id] = {size, freq, GHOST, ghost_.begin()};
        g_size_bytes_ += size;

        while (g_size_bytes_ > cache_size_ && !ghost_.empty()) {
            obj_id_t victim = ghost_.back();
            ghost_.pop_back();
            auto it = meta_.find(victim);
            if (it != meta_.end()) {
                g_size_bytes_ -= it->second.size;
                meta_.erase(it);
            }
        }
    }

    void on_hit(obj_id_t id) {
        auto it = meta_.find(id);
        if (it != meta_.end() && it->second.q_type != GHOST) {
            auto &freq = it->second.freq;
            freq = std::min<uint8_t>(freq + 1, 3);
        }
    }

    void on_miss(obj_id_t id, uint64_t size) {
        if (size > cache_size_) {
            return;
        }

        auto it = meta_.find(id);
        if (it != meta_.end() && it->second.q_type == GHOST) {
            g_size_bytes_ -= it->second.size;
            ghost_.erase(it->second.it);
            meta_.erase(it);
            push_main(id, size, 0);
        } else {
            push_small(id, size, 0);
        }
    }

    std::optional<obj_id_t> evict_main() {
        while (!main_.empty()) {
            obj_id_t t = main_.back();
            main_.pop_back();
            auto it = meta_.find(t);
            assert(it != meta_.end());

            uint64_t t_size = it->second.size;
            uint8_t t_freq = it->second.freq;
            m_size_bytes_ -= t_size;
            meta_.erase(t);

            if (t_freq > 0) {
                push_main(t, t_size, t_freq - 1);
            } else {
                return t;
            }
        }
        return {};
    }

    std::optional<obj_id_t> evict_small() {
        while (!small_.empty()) {
            obj_id_t t = small_.back();
            small_.pop_back();
            auto it = meta_.find(t);
            assert(it != meta_.end());

            uint64_t t_size = it->second.size;
            uint8_t t_freq = it->second.freq;
            s_size_bytes_ -= t_size;
            meta_.erase(it);

            if (t_freq > 0) {
                push_main(t, t_size, 0);
            } else {
                push_ghost(t, t_size, 0);
                return t;
            }
        }
        return {};
    }

    obj_id_t evict() {
        uint64_t target_s_size = cache_size_ / 10;
        while (true) {
            if (s_size_bytes_ >= target_s_size) {
                if (auto victim = evict_small()) {
                    return *victim;
                }
            }

            if (auto victim = evict_main()) {
                return *victim;
            }

            if (auto victim = evict_small()) {
                return *victim;
            }
        }

        return 0;
    }

    void on_remove(obj_id_t id) {
        auto it = meta_.find(id);
        if (it != meta_.end()) {
            if (it->second.q_type == SMALL) {
                small_.erase(it->second.it);
                s_size_bytes_ -= it->second.size;
            } else if (it->second.q_type == MAIN) {
                main_.erase(it->second.it);
                m_size_bytes_ -= it->second.size;
            } else if (it->second.q_type == GHOST) {
                ghost_.erase(it->second.it);
                g_size_bytes_ -= it->second.size;
            }
            meta_.erase(it);
        }
    }
};

extern "C" {
void *cache_init_hook(const common_cache_params_t params) {
    return new S3FifoCache(params.cache_size);
}

void cache_hit_hook(void *data, const request_t *req) {
    static_cast<S3FifoCache *>(data)->on_hit(req->obj_id);
}

void cache_miss_hook(void *data, const request_t *req) {
    static_cast<S3FifoCache *>(data)->on_miss(req->obj_id, req->obj_size);
}

obj_id_t cache_eviction_hook(void *data, const request_t * /*req*/) {
    return static_cast<S3FifoCache *>(data)->evict();
}

void cache_remove_hook(void *data, obj_id_t obj_id) {
    static_cast<S3FifoCache *>(data)->on_remove(obj_id);
}

void cache_free_hook(void *data) {
    S3FifoCache *fifo_cache = (S3FifoCache *)data;
    delete fifo_cache;
}
} // extern "C"
