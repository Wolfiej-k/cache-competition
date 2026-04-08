#include <cassert>
#include <cmath>
#include <libCacheSim.h>
#include <list>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

class IExpert {
  public:
    virtual ~IExpert() = default;
    virtual bool contains(obj_id_t id) const = 0;
    virtual void on_hit(obj_id_t id) = 0;
    virtual void on_miss(obj_id_t id, uint64_t size) = 0;
    virtual obj_id_t evict() = 0;
    virtual void on_remove(obj_id_t id) = 0;
};

template <typename T> class ExpertWrapper : public IExpert {
  private:
    T expert_cache_;

  public:
    ExpertWrapper(uint64_t cache_size) : expert_cache_(cache_size) {}
    bool contains(obj_id_t id) const override {
        return expert_cache_.contains(id);
    }
    void on_hit(obj_id_t id) override { expert_cache_.on_hit(id); }
    void on_miss(obj_id_t id, uint64_t size) override {
        expert_cache_.on_miss(id, size);
    }
    obj_id_t evict() override { return expert_cache_.evict(); }
    void on_remove(obj_id_t id) override { expert_cache_.on_remove(id); }
};

class SwitchingCache {
  private:
    std::vector<std::unique_ptr<IExpert>> experts_;
    std::vector<double> weights_;

    std::unordered_map<obj_id_t, uint64_t> physical_meta_;
    uint64_t physical_size_bytes_;
    uint64_t cache_capacity_;

    double learning_rate_;
    std::mt19937 rng_;

    void normalize_weights() {
        double sum = std::accumulate(weights_.begin(), weights_.end(), 0.0);
        for (double &w : weights_) {
            w /= sum;
        }
    }

  public:
    SwitchingCache(uint64_t capacity, double learning_rate = 0.45)
        : cache_capacity_(capacity), physical_size_bytes_(0),
          learning_rate_(learning_rate) {
        std::random_device rd;
        rng_.seed(rd());
    }

    template <typename T> void add_expert() {
        experts_.push_back(std::make_unique<ExpertWrapper<T>>(cache_capacity_));
        weights_.resize(experts_.size(), 1.0);
        normalize_weights();
    }

    void on_hit(obj_id_t id) {
        for (auto &expert : experts_) {
            if (expert->contains(id)) {
                expert->on_hit(id);
            } else {
                uint64_t size = physical_meta_[id];
                expert->on_miss(id, size);
            }
        }
    }

    void on_miss(obj_id_t id, uint64_t size) {
        if (size > cache_capacity_) {
            return;
        }

        bool weights_changed = false;
        for (size_t i = 0; i < experts_.size(); ++i) {
            if (experts_[i]->contains(id)) {
                weights_[i] *= std::exp(learning_rate_);
                weights_changed = true;
            }
        }

        if (weights_changed) {
            normalize_weights();
        }

        for (auto &expert : experts_) {
            if (expert->contains(id)) {
                expert->on_hit(id);
            } else {
                expert->on_miss(id, size);
            }
        }

        physical_meta_[id] = size;
        physical_size_bytes_ += size;
    }

    obj_id_t evict() {
        std::discrete_distribution<size_t> dist(weights_.begin(),
                                                weights_.end());
        size_t chosen_expert_idx = dist(rng_);

        obj_id_t physical_victim = 0;
        while (true) {
            obj_id_t nominated_victim = experts_[chosen_expert_idx]->evict();
            auto it = physical_meta_.find(nominated_victim);
            if (it != physical_meta_.end()) {
                physical_victim = nominated_victim;
                physical_size_bytes_ -= it->second;
                physical_meta_.erase(it);
                break;
            }
        }

        return physical_victim;
    }

    void on_remove(obj_id_t id) {
        auto it = physical_meta_.find(id);
        if (it != physical_meta_.end()) {
            physical_size_bytes_ -= it->second;
            physical_meta_.erase(it);
        }

        for (auto &expert : experts_) {
            expert->on_remove(id);
        }
    }
};

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

class S3SieveCache {
  private:
    enum QueueType : uint8_t { NONE, SMALL, MAIN, GHOST };

    struct ObjectMeta {
        uint64_t size;
        uint8_t freq;
        QueueType q_type;
        std::list<obj_id_t>::iterator it;
    };

    std::list<obj_id_t> small_;
    SieveQueue<obj_id_t> main_;
    std::list<obj_id_t> ghost_;

    std::unordered_map<obj_id_t, ObjectMeta> meta_;

    uint64_t cache_size_;
    uint64_t s_size_bytes_;
    uint64_t g_size_bytes_;

    bool can_fit(uint64_t size) const noexcept {
        return s_size_bytes_ + main_.get_size_bytes() + size <= cache_size_;
    }

    void push_main(obj_id_t id, uint64_t size) {
        main_.push(id, size);
        meta_[id] = {size, 0, MAIN, {}};
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

  public:
    S3SieveCache(uint64_t cache_size)
        : cache_size_(cache_size), s_size_bytes_(0), g_size_bytes_(0) {}

    bool contains(obj_id_t id) const {
        auto it = meta_.find(id);
        return it != meta_.end() && it->second.q_type != GHOST;
    }

    void on_hit(obj_id_t id) {
        auto it = meta_.find(id);
        if (it != meta_.end()) {
            if (it->second.q_type == MAIN) {
                main_.mark_visited(id);
            } else if (it->second.q_type == SMALL) {
                auto &freq = it->second.freq;
                freq = std::min<uint8_t>(freq + 1, 3);
            }
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
            push_main(id, size);
        } else {
            push_small(id, size, 0);
        }
    }

    std::optional<obj_id_t> evict_main() {
        if (auto victim = main_.evict()) {
            meta_.erase(*victim);
            return victim;
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
                push_main(t, t_size);
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
                main_.remove(id);
            } else if (it->second.q_type == GHOST) {
                ghost_.erase(it->second.it);
                g_size_bytes_ -= it->second.size;
            }
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

    bool contains(obj_id_t id) const { return sieve_.contains(id); }

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
    SwitchingCache *master = new SwitchingCache(params.cache_size, 0.45);
    master->add_expert<S3SieveCache>();
    master->add_expert<SieveCache>();
    return master;
}

void cache_hit_hook(void *data, const request_t *req) {
    static_cast<SwitchingCache *>(data)->on_hit(req->obj_id);
}

void cache_miss_hook(void *data, const request_t *req) {
    static_cast<SwitchingCache *>(data)->on_miss(req->obj_id, req->obj_size);
}

obj_id_t cache_eviction_hook(void *data, const request_t * /*req*/) {
    return static_cast<SwitchingCache *>(data)->evict();
}

void cache_remove_hook(void *data, obj_id_t obj_id) {
    static_cast<SwitchingCache *>(data)->on_remove(obj_id);
}

void cache_free_hook(void *data) { delete static_cast<SwitchingCache *>(data); }
} // extern "C"
