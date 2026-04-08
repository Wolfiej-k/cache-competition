#include "sieve_queue.hpp"
#include <libCacheSim.h>
#include <optional>

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
