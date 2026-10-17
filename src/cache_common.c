/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdatomic.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/sysctl.h>

#include "cache.h"
#include "cache_internal.h"

/**
 * @brief Doubles the capacity of the internal key and value-pointer hash
 *        tables when the load factor threshold is exceeded.
 * @details Re-indexes all active entries across the expanded key buckets
 *          (using cached key hashes) and value-pointer buckets while holding
 *          all stripe locks and the LRU lock.
 * @param cache A valid cache instance pointer.
 */
static void
cache_rehash_expand_internal(cache_t *cache)
{
    cache_bucket_t *new_buckets, *old_buckets;
    cache_entry_t *curr, *next_node;
    size_t old_count, new_count, new_mask, hash_val, val_slot, i;

    /* Calculate the expanded hash table dimensions (2x current capacity) */
    old_count = cache->bucket_count;
    new_count = old_count << 1;
    new_mask = new_count - 1;

    new_buckets = calloc(new_count, sizeof(cache_bucket_t));
    if (new_buckets == NULL)
        return;

    cache_lock_stripes(cache);
    cache_lock_lru(cache);

    /* Abort if another thread already expanded the table while waiting for locks */
    if (cache->bucket_count != old_count) {
        cache_unlock_lru(cache);
        cache_unlock_stripes(cache);
        free(new_buckets);
        return;
    }

    old_buckets = cache->buckets;

    /* Rehash all active entries into the expanded key bucket chains */
    for (i = 0; i < old_count; i++) {
        curr = old_buckets[i].key_head;
        while (curr != NULL) {
            next_node = curr->next;
            hash_val = curr->key_hash & new_mask;
            curr->next = new_buckets[hash_val].key_head;
            new_buckets[hash_val].key_head = curr;
            curr = next_node;
        }
    }

    /* Rehash all entries (active and deferred) into the expanded value bucket chains */
    for (i = 0; i < old_count; i++) {
        curr = old_buckets[i].val_head;
        while (curr != NULL) {
            next_node = curr->val_next;
            val_slot = cache_hash_pointer(curr->value) & new_mask;
            curr->val_next = new_buckets[val_slot].val_head;
            new_buckets[val_slot].val_head = curr;
            curr = next_node;
        }
    }

    /* Install the expanded bucket array, count, and mask */
    cache->buckets = new_buckets;
    cache->bucket_count = new_count;
    cache->bucket_mask = new_mask;

    cache_unlock_lru(cache);
    cache_unlock_stripes(cache);

    free(old_buckets);
}

/**
 * @brief Queries total physical system memory in bytes.
 * @return Total bytes of physical memory, or a fallback default if query fails.
 */
static uint64_t
cache_query_physical_memory(void)
{
    uint64_t mem_bytes = 0;
    int mib[2] = { CTL_HW, HW_MEMSIZE };
    size_t len = sizeof(mem_bytes);

    if (sysctl(mib, 2, &mem_bytes, &len, NULL, 0) != 0 || mem_bytes == 0) {
        mem_bytes = CACHE_DEFAULT_PHYSICAL_MEMORY_BYTES;
    }

    return (mem_bytes);
}

/**
 * @brief Creates and initializes a new cache instance with the specified
 *        attributes and callbacks.
 * @details Normalizes attribute version (falling through and assuming
 *          CACHE_ATTRIBUTES_VERSION_1 if not CACHE_ATTRIBUTES_VERSION_2),
 *          defaults NULL key_hash_cb and key_is_equal_cb to integer/pointer
 *          callbacks, allocates lock stripes and hash buckets, and registers
 *          low-memory pressure handlers.
 * @param name Descriptive label for the cache instance used for diagnostics.
 * @param attrs Pointer to the cache configuration attributes and lifecycle
 *              callbacks.
 * @param cache_out Output pointer where the newly created cache instance
 *                  handle is written.
 * @return 0 on success, EINVAL if name, attrs, or cache_out is NULL, or
 *         ENOMEM if memory allocation fails.
 */
CACHE_PUBLIC_API int
cache_create(const char *name, cache_attributes_t *attrs, cache_t **cache_out)
{
    cache_t *cache;
    dispatch_queue_t monitor_queue;
    size_t initial_hint;
    int i, ncpu;

    if (name == NULL || cache_out == NULL || attrs == NULL)
        return (EINVAL);

    cache = calloc(1, sizeof(cache_t));
    if (cache == NULL)
        return (ENOMEM);

    cache->name = strdup(name);
    if (cache->name == NULL) {
        free(cache);
        return (ENOMEM);
    }
    
    /* Query logical CPU count to size the lock stripe array */
    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1)
        ncpu = 4;

    /* Round stripe count up to a power of two (at least 2 * ncpu) */
    cache->num_stripes = 1;
    while (cache->num_stripes < (size_t)ncpu) {
        cache->num_stripes <<= 1;
    }
    cache->num_stripes <<= 1;
    cache->stripe_mask = cache->num_stripes - 1;

    /* Allocate and initialize stripe locks and the LRU lock */
    cache->locks = calloc(cache->num_stripes, sizeof(os_unfair_lock));
    if (cache->locks == NULL) {
        free(cache->name);
        free(cache);
        return (ENOMEM);
    }
    for (i = 0; i < (int)cache->num_stripes; i++)
        cache->locks[i] = OS_UNFAIR_LOCK_INIT;

    cache->lru_lock = OS_UNFAIR_LOCK_INIT;

    /* Copy attributes and apply default integer key callbacks if omitted */
    cache->attrs.version = CACHE_ATTRIBUTES_VERSION_1;
    cache->attrs.key_hash_cb = cache_key_hash_cb_integer;
    cache->attrs.key_is_equal_cb = cache_key_is_equal_cb_integer;
    cache->attrs.key_retain_cb = attrs->key_retain_cb;
    cache->attrs.key_release_cb = attrs->key_release_cb;
    cache->attrs.value_release_cb = attrs->value_release_cb;
    cache->attrs.value_make_nonpurgeable_cb = attrs->value_make_nonpurgeable_cb;
    cache->attrs.value_make_purgeable_cb = attrs->value_make_purgeable_cb;
    cache->attrs.user_data = attrs->user_data;

    if (attrs->key_hash_cb != NULL)
        cache->attrs.key_hash_cb = attrs->key_hash_cb;

    if (attrs->key_is_equal_cb != NULL)
        cache->attrs.key_is_equal_cb = attrs->key_is_equal_cb;

    if (attrs->version == CACHE_ATTRIBUTES_VERSION_2) {
        cache->attrs.version = attrs->version;
        cache->attrs.value_retain_cb = attrs->value_retain_cb;
    }

    /* Bind cache entry allocations to the default purgeable malloc zone */
    cache->memory_zone = malloc_default_purgeable_zone();

    /* Size initial hash tables as a power of two at least as large as num_stripes */
    initial_hint = 16;
    cache->bucket_count = 16;
    while (cache->bucket_count < initial_hint ||
           cache->bucket_count < cache->num_stripes) {
        cache->bucket_count <<= 1;
    }
    cache->bucket_mask = cache->bucket_count - 1;

    cache->buckets = calloc(cache->bucket_count, sizeof(cache_bucket_t));
    if (cache->buckets == NULL) {
        free(cache->locks);
        free(cache->name);
        free(cache);
        return (ENOMEM);
    }

    /* Initialize atomic metrics, hints, reference count, and teardown state */
    atomic_init(&cache->current_cost, 0);
    atomic_init(&cache->current_count, 0);
    atomic_init(&cache->minimum_values_hint, 0);
    atomic_init(&cache->configured_count_hint, 0);
    atomic_init(&cache->ref_count, 1);
    atomic_init(&cache->is_destroying, false);

    /* Initialize dynamic memory-proportional capacity limit */
    {
        uint64_t sys_mem = cache_query_physical_memory();
        size_t dynamic_limit = (size_t)(sys_mem / CACHE_BYTES_PER_DYNAMIC_ENTRY);
        if (dynamic_limit < CACHE_MIN_DYNAMIC_CAPACITY_LIMIT)
            dynamic_limit = CACHE_MIN_DYNAMIC_CAPACITY_LIMIT;
        cache->base_capacity_limit = dynamic_limit;
        atomic_init(&cache->dynamic_capacity_limit, dynamic_limit);
    }

    /* Create a serial queue targeting the low-priority global queue for memory-pressure events */
    monitor_queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0);
    cache->monitor_queue = dispatch_queue_create("com.darwin.libcache.monitor",
        DISPATCH_QUEUE_SERIAL);
    if (cache->monitor_queue != NULL) {
        dispatch_set_target_queue(cache->monitor_queue, monitor_queue);

        cache->mem_pressure_source = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_MEMORYPRESSURE,
            0,
            DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
            cache->monitor_queue
        );
        cache->sim_pressure_source = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_DATA_OR,
            0,
            0,
            cache->monitor_queue
        );

        if (cache->mem_pressure_source != NULL &&
            cache->sim_pressure_source != NULL) {
            dispatch_source_t pressure_source = cache->mem_pressure_source;
            dispatch_source_t sim_source = cache->sim_pressure_source;
            __block cache_t *weak_cache = cache;
            dispatch_block_t pressure_handler = ^{
                unsigned long raw_flags;
                dispatch_source_memorypressure_flags_t pressure_flags;

                /*
                 * dispatch_source_cancel() is asynchronous and does not
                 * preempt or dequeue an event handler block that has already
                 * been scheduled onto cache->monitor_queue. Checking
                 * is_destroying ensures that any handler block drained by
                 * cache_destroy()'s dispatch_sync(cache->monitor_queue, ...)
                 * exits immediately without touching cache state during
                 * teardown.
                 */
                if (atomic_load_explicit(&weak_cache->is_destroying,
                    memory_order_acquire) ||
                    dispatch_source_testcancel(pressure_source) != 0 ||
                    dispatch_source_testcancel(sim_source) != 0) {
                    return;
                }

                raw_flags = dispatch_source_get_data(pressure_source) |
                    dispatch_source_get_data(sim_source);
                pressure_flags = (dispatch_source_memorypressure_flags_t)raw_flags;

                if (pressure_flags & (DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL | DISPATCH_MEMORYPRESSURE_NORMAL)) {
                    _cache_handle_memory_pressure_event(weak_cache, pressure_flags);
                }
            };
            dispatch_source_set_event_handler(pressure_source,
                pressure_handler);
            dispatch_source_set_event_handler(sim_source, pressure_handler);
            dispatch_resume(pressure_source);
            dispatch_resume(sim_source);
            _cache_sim_pressure_enroll(cache);
        }
    }

    *cache_out = cache;
    return (0);
}

/**
 * @brief Inserts or updates a key-value entry in the cache and retains a
 *        reference to a non-NULL value.
 * @details New entries with a non-NULL value are inserted with a retain
 *          count of 1; if value is NULL, the key is associated with NULL and
 *          retain count 0. If the key already exists with the same value
 *          pointer, its retain count is incremented (if non-NULL) and its
 *          cost and LRU position are updated. If the key exists with a
 *          different value, the previous entry is replaced (and deferred if
 *          still retained).
 * @param cache A valid cache instance.
 * @param key Non-NULL key identifying the entry.
 * @param value Value pointer to store and retain, or NULL to associate the
 *              key with NULL.
 * @param cost Cost weight associated with the entry for eviction accounting.
 * @return 0 on success, EINVAL if cache or key is NULL, or ENOMEM if memory
 *         allocation fails.
 */
CACHE_PUBLIC_API int
cache_set_and_retain(cache_t *cache, void *key, void *value, cache_cost_t cost)
{
    cache_entry_t *entry, *curr, *tail_node;
    os_unfair_lock_t stripe_lock, tail_stripe_lock;
    void *key_copy, *value_store, *old_key;
    uintptr_t hash_val, tail_hash_val;
    size_t count_limit, live_count, current_capacity;

    if (cache == NULL || key == NULL)
        return (EINVAL);

    hash_val = CACHE_KEY_HASH(cache, key);
    key_copy = NULL;

    /* Retain incoming key (if configured) */
    if (cache->attrs.key_retain_cb != NULL) {
        cache->attrs.key_retain_cb(key, &key_copy, cache->attrs.user_data);
        if (key_copy == NULL)
            return (EINVAL);
    } else {
        key_copy = key;
    }

    /* Retain incoming value for VERSION_2 attributes (if configured) */
    value_store = value;
    if (cache->attrs.version == CACHE_ATTRIBUTES_VERSION_2 &&
        cache->attrs.value_retain_cb != NULL) {
        cache->attrs.value_retain_cb(value_store, cache->attrs.user_data);
    }

    /* Allocate and initialize entry metadata */
    entry = malloc_zone_malloc(cache->memory_zone, sizeof(cache_entry_t));
    if (entry == NULL) {
        if (cache->attrs.version == CACHE_ATTRIBUTES_VERSION_2 &&
            cache->attrs.value_retain_cb != NULL &&
            cache->attrs.value_release_cb != NULL) {
            cache->attrs.value_release_cb(value_store, cache->attrs.user_data);
        }
        if (cache->attrs.key_release_cb != NULL) {
            cache->attrs.key_release_cb(key_copy, cache->attrs.user_data);
        }
        return (ENOMEM);
    }
    entry->key = key_copy;
    entry->value = value_store;
    entry->key_hash = hash_val;
    entry->cost = cost;
    entry->retain_count = (value_store != NULL) ? 1 : 0;
    entry->is_removed = false;
    entry->is_protected = false;
    entry->next = NULL;
    entry->val_next = NULL;
    entry->lru_next = NULL;
    entry->lru_prev = NULL;

    stripe_lock = GET_STRIPE_LOCK(cache, hash_val);
    os_unfair_lock_lock(stripe_lock);

    /* Check if an entry for this key already exists and replace or update it */
    curr = GET_KEY_BUCKET(cache, hash_val);
    while (curr != NULL) {
        if (CACHE_KEY_MATCH(cache, curr, key, hash_val)) {
            if (curr->value == value_store) {
                /* Same value pointer: update existing entry in place and free new node */
                old_key = curr->key;
                curr->key = key_copy;

                cache_lock_lru(cache);
                if (value_store != NULL) {
                    if (curr->retain_count == 0 &&
                        cache->attrs.value_make_nonpurgeable_cb != NULL) {
                        (void)cache->attrs.value_make_nonpurgeable_cb(curr->value,
                            cache->attrs.user_data);
                    }
                    curr->retain_count++;
                    cache_retain(cache);
                }
                if (cost > curr->cost) {
                    atomic_fetch_add_explicit(&cache->current_cost,
                        cost - curr->cost, memory_order_relaxed);
                } else if (cost < curr->cost) {
                    atomic_fetch_sub_explicit(&cache->current_cost,
                        curr->cost - cost, memory_order_relaxed);
                }
                curr->cost = cost;

                entry->key = (old_key != key_copy) ? old_key : NULL;
                entry->value = NULL;
                free_entry_resources(cache, entry);

                touch_entry(cache, curr);
                cache_unlock_lru(cache);
                os_unfair_lock_unlock(stripe_lock);
                return (0);
            }

            /* Different value pointer: break to unlink old entry and insert new one */
            break;
        }
        curr = curr->next;
    }

    /* Lock LRU once to handle both replacement unlinking and new entry insertion */
    cache_lock_lru(cache);
    if (curr != NULL) {
        if (curr->retain_count > 0) {
            mark_entry_removed(cache, curr);
        } else {
            detach_entry(cache, curr);
            free_entry_resources(cache, curr);
        }
    }
    /* Insert the new entry at the head of its key hash bucket */
    entry->next = GET_KEY_BUCKET(cache, hash_val);
    GET_KEY_BUCKET(cache, hash_val) = entry;
    if (value_store != NULL) {
        cache_retain(cache);
    }
    attach_probation_head(cache, entry);
    attach_val_bucket(cache, entry);
    cache_unlock_lru(cache);

    /* Update aggregate cost and entry count */
    atomic_fetch_add_explicit(&cache->current_cost, cost, memory_order_relaxed);
    live_count = atomic_fetch_add_explicit(&cache->current_count, 1,
        memory_order_relaxed) + 1;
    current_capacity = cache->bucket_count;
    os_unfair_lock_unlock(stripe_lock);

    /* Expand hash tables when entry count exceeds bucket capacity */
    if (live_count > current_capacity && current_capacity < CACHE_MAX_BUCKET_COUNT) {
        cache_rehash_expand_internal(cache);
    }

    /* Evict an unretained entry if capacity limit is exceeded (SLRU: probationary tail first, then protected tail) */
    count_limit = cache_get_effective_count_limit(cache);
    if (count_limit > 0 && live_count > count_limit) {
        cache_lock_lru(cache);
        
        /* First try to evict an unretained entry from probationary segment (LRU tail to head) */
        tail_node = cache->prob_tail;
        while (tail_node != NULL && tail_node->retain_count > 0) {
            tail_node = tail_node->lru_prev;
        }

        /* If no unretained entry in probationary segment, search protected segment */
        if (tail_node == NULL) {
            tail_node = cache->prot_tail;
            while (tail_node != NULL && tail_node->retain_count > 0) {
                tail_node = tail_node->lru_prev;
            }
        }

        if (tail_node != NULL) {
            tail_hash_val = tail_node->key_hash;
            tail_stripe_lock = GET_STRIPE_LOCK(cache, tail_hash_val);

            /* Use trylock to avoid lock-order inversion while holding lru_lock */
            if (os_unfair_lock_trylock(tail_stripe_lock)) {
                if (tail_node->retain_count == 0) {
                    detach_entry(cache, tail_node);
                    free_entry_resources(cache, tail_node);
                    cache_unlock_lru(cache);
                    os_unfair_lock_unlock(tail_stripe_lock);
                    return (0);
                }
                os_unfair_lock_unlock(tail_stripe_lock);
            }
        }
        cache_unlock_lru(cache);
    }

    return (0);
}

/**
 * @brief Looks up a value by key and increments its retain count to prevent
 *        eviction while in use.
 * @details Sets *value_out to NULL on a cache miss. If an unretained entry's
 *          value was purged (value_make_nonpurgeable_cb returns false), the
 *          entry is evicted and ENOENT is returned.
 * @param cache A valid cache instance.
 * @param key Non-NULL key to look up.
 * @param value_out Output pointer where the matching value pointer is
 *                  written (or NULL if not found).
 * @return 0 on success, EINVAL if cache, key, or value_out is NULL, or
 *         ENOENT if the key is not found or was purged.
 */
CACHE_PUBLIC_API int
cache_get_and_retain(cache_t *cache, void *key, void **value_out)
{
    cache_entry_t *curr;
    os_unfair_lock_t stripe_lock;
    uintptr_t hash_val;

    if (cache == NULL || key == NULL || value_out == NULL)
        return (EINVAL);

    *value_out = NULL;

    hash_val = CACHE_KEY_HASH(cache, key);
    stripe_lock = GET_STRIPE_LOCK(cache, hash_val);

    os_unfair_lock_lock(stripe_lock);
    curr = GET_KEY_BUCKET(cache, hash_val);

    /* Search the bucket chain for a matching key */
    while (curr != NULL) {
        if (CACHE_KEY_MATCH(cache, curr, key, hash_val)) {
            /* If unretained, mark value non-purgeable and evict if it was purged */
            cache_lock_lru(cache);
            if (curr->value != NULL) {
                if (curr->retain_count == 0 &&
                    cache->attrs.value_make_nonpurgeable_cb != NULL) {
                    if (!cache->attrs.value_make_nonpurgeable_cb(curr->value,
                        cache->attrs.user_data)) {
                        detach_entry(cache, curr);
                        free_entry_resources(cache, curr);
                        cache_unlock_lru(cache);
                        os_unfair_lock_unlock(stripe_lock);
                        return (ENOENT);
                    }
                }

                /* Retain the entry to prevent eviction while held by the caller */
                curr->retain_count++;
                cache_retain(cache);
            }
            *value_out = curr->value;
            
            /* Promote the entry via SLRU rules */
            touch_entry(cache, curr);
            cache_unlock_lru(cache);
            os_unfair_lock_unlock(stripe_lock);
            return (0);
        }
        curr = curr->next;
    }
    os_unfair_lock_unlock(stripe_lock);
    return (ENOENT);
}

/**
 * @brief Releases a previously retained reference to a cached value.
 * @details Decrements the entry's retain count. When the retain count drops
 *          to 0, value_make_purgeable_cb is invoked for active entries, or
 *          the entry is finalized and freed if it was previously removed
 *          while retained. If value is NULL, returns 0 without modifying
 *          cache state.
 * @param cache A valid cache instance.
 * @param value Value pointer previously retained via cache_set_and_retain or
 *              cache_get_and_retain, or NULL.
 * @return 0 on success (or if value is NULL), EINVAL if cache is NULL, or
 *         ENOENT if a non-NULL value is not tracked by the cache.
 */
CACHE_PUBLIC_API int
cache_release_value(cache_t *cache, void *value)
{
    cache_entry_t *curr;
    size_t val_hash;

    if (cache == NULL)
        return (EINVAL);

    if (value == NULL)
        return (0);

    /* Fast O(1) lookup via value-pointer hash index under lru_lock */
    cache_lock_lru(cache);
    val_hash = cache_hash_pointer(value);
    curr = GET_VAL_BUCKET(cache, val_hash);
    while (curr != NULL) {
        if (curr->value == value) {
            if (curr->retain_count > 0) {
                curr->retain_count--;
                if (curr->retain_count == 0) {
                    if (curr->is_removed) {
                        detach_val_bucket(cache, curr);
                        free_entry_resources(cache, curr);
                        cache_unlock_lru(cache);
                        cache_release(cache);
                        return (0);
                    }
                    if (cache->attrs.value_make_purgeable_cb != NULL) {
                        cache->attrs.value_make_purgeable_cb(
                            curr->value, cache->attrs.user_data);
                    }
                }
                cache_unlock_lru(cache);
                cache_release(cache);
                return (0);
            }
            cache_unlock_lru(cache);
            return (0);
        }
        curr = curr->val_next;
    }
    cache_unlock_lru(cache);

    return (ENOENT);
}

/**
 * @brief Removes the entry associated with the specified key from the cache.
 * @details Unretained entries are finalized and freed immediately. If the
 *          entry has a non-zero retain count, it is immediately unlinked
 *          from lookups and deferred until its last reference is released
 *          via cache_release_value.
 * @param cache A valid cache instance.
 * @param key Non-NULL key identifying the entry to remove.
 * @return 0 on success, EINVAL if cache or key is NULL, or ENOENT if the key
 *         is not found.
 */
CACHE_PUBLIC_API int
cache_remove(cache_t *cache, void *key)
{
    cache_entry_t *curr;
    os_unfair_lock_t stripe_lock;
    uintptr_t hash_val;

    if (cache == NULL || key == NULL)
        return (EINVAL);

    hash_val = CACHE_KEY_HASH(cache, key);
    stripe_lock = GET_STRIPE_LOCK(cache, hash_val);

    os_unfair_lock_lock(stripe_lock);
    curr = GET_KEY_BUCKET(cache, hash_val);
    while (curr != NULL) {
        if (CACHE_KEY_MATCH(cache, curr, key, hash_val)) {
            /* Unlink the entry from hash buckets, LRU queue, and update metrics */
            cache_lock_lru(cache);
            if (curr->retain_count > 0) {
                mark_entry_removed(cache, curr);
            } else {
                detach_entry(cache, curr);
                free_entry_resources(cache, curr);
            }
            cache_unlock_lru(cache);
            os_unfair_lock_unlock(stripe_lock);
            return (0);
        }
        curr = curr->next;
    }
    os_unfair_lock_unlock(stripe_lock);
    return (ENOENT);
}

/**
 * @brief Evicts all live entries from the cache while all stripe locks and
 *        the LRU lock are held.
 * @details Unretained entries are finalized and freed, while currently
 *          retained entries remain indexed by value for deferred O(1)
 *          finalization via cache_release_value.
 * @param cache A valid locked cache instance.
 */
static void
cache_remove_all_locked(cache_t *cache)
{
    cache_entry_t *curr, *next;
    size_t b;

    /* Unlink all key bucket heads so key lookups miss immediately */
    for (b = 0; b < cache->bucket_count; b++) {
        cache->buckets[b].key_head = NULL;
    }

    /* Process all active entries across protected and probationary segments */
    cache_entry_t *seg_heads[2];
    int s;

    seg_heads[0] = cache->prot_head;
    seg_heads[1] = cache->prob_head;

    cache->prot_head = NULL;
    cache->prot_tail = NULL;
    cache->prot_count = 0;
    cache->prob_head = NULL;
    cache->prob_tail = NULL;
    cache->prob_count = 0;
    cache->head = NULL;
    cache->tail = NULL;

    for (s = 0; s < 2; s++) {
        curr = seg_heads[s];
        while (curr != NULL) {
            next = curr->lru_next;
            curr->lru_prev = NULL;
            curr->lru_next = NULL;
            curr->next = NULL;
            if (curr->retain_count > 0) {
                curr->is_removed = true;
            } else {
                detach_val_bucket(cache, curr);
                free_entry_resources(cache, curr);
            }
            curr = next;
        }
    }

    /* Sweep any unretained entries from value buckets */
    for (b = 0; b < cache->bucket_count; b++) {
        cache_entry_t *val_curr = cache->buckets[b].val_head;
        cache_entry_t *val_prev = NULL;
        while (val_curr != NULL) {
            cache_entry_t *val_next = val_curr->val_next;
            if (val_curr->retain_count == 0) {
                if (val_prev != NULL)
                    val_prev->val_next = val_next;
                else
                    cache->buckets[b].val_head = val_next;
                val_curr->val_next = NULL;
                free_entry_resources(cache, val_curr);
            } else {
                val_curr->is_removed = true;
                val_prev = val_curr;
            }
            val_curr = val_next;
        }
    }

    cache->current_cost = 0;
    cache->current_count = 0;
}

/**
 * @brief Removes all entries from the cache and resets aggregate cost and
 *        count metrics to zero.
 * @details Unretained entries are finalized and freed immediately, while
 *          currently retained entries are unlinked and deferred until
 *          released via cache_release_value.
 * @param cache A valid cache instance.
 * @return 0 on success, or EINVAL if cache is NULL.
 */
CACHE_PUBLIC_API int
cache_remove_all(cache_t *cache)
{
    if (cache == NULL)
        return (EINVAL);

    cache_lock_stripes(cache);
    cache_lock_lru(cache);

    cache_remove_all_locked(cache);

    cache_unlock_lru(cache);
    cache_unlock_stripes(cache);

    return (0);
}

/**
 * @brief Scans the cache and removes all unretained entries for which the
 *        supplied predicate block returns true.
 * @param cache A valid cache instance.
 * @param remove_block Block predicate evaluated for each unretained entry;
 *                     returning true removes the entry.
 * @return Total number of unretained entries removed, or EINVAL if cache or
 *         remove_block is NULL.
 */
CACHE_PRIVATE_API int
cache_remove_with_block(cache_t *cache, cache_remove_block_t remove_block)
{
    cache_entry_t *curr, *next_entry;
    size_t b, active_capacity;
    int total_removed;

    if (cache == NULL || remove_block == NULL)
        return (EINVAL);
    total_removed = 0;

    cache_lock_stripes(cache);
    cache_lock_lru(cache);

    active_capacity = cache->bucket_count;

    /* Scan all key buckets and evaluate remove_block on unretained entries */
    for (b = 0; b < active_capacity; b++) {
        curr = cache->buckets[b].key_head;

        while (curr != NULL) {
            next_entry = curr->next;

            /* Remove unretained entries for which remove_block returns true */
            if (curr->retain_count == 0 &&
                remove_block(curr->key, curr->value)) {
                detach_entry(cache, curr);
                free_entry_resources(cache, curr);
                total_removed++;
            }
            curr = next_entry;
        }
    }

    cache_unlock_lru(cache);
    cache_unlock_stripes(cache);

    return (total_removed);
}

/**
 * @brief Retrieves a value by key and promotes its LRU position without
 *        incrementing its retain count.
 * @param cache A valid cache instance.
 * @param key Non-NULL key to query.
 * @param value_out Output pointer where the discovered value pointer is
 *                  written (or NULL if not found).
 * @return 0 on success, EINVAL if cache, key, or value_out is NULL, or
 *         ENOENT if the key is not found.
 */
CACHE_PRIVATE_API int
cache_get(cache_t *cache, void *key, void **value_out)
{
    cache_entry_t *curr;
    os_unfair_lock_t stripe_lock;
    uintptr_t hash_val;

    if (cache == NULL || key == NULL || value_out == NULL)
        return (EINVAL);

    *value_out = NULL;

    hash_val = CACHE_KEY_HASH(cache, key);
    stripe_lock = GET_STRIPE_LOCK(cache, hash_val);

    os_unfair_lock_lock(stripe_lock);
    curr = GET_KEY_BUCKET(cache, hash_val);
    while (curr != NULL) {
        if (CACHE_KEY_MATCH(cache, curr, key, hash_val)) {
            
            /* Return the value pointer without modifying the entry's retain count */
            *value_out = curr->value;
            
            /* Promote the entry via SLRU rules */
            cache_lock_lru(cache);
            touch_entry(cache, curr);
            cache_unlock_lru(cache);
            os_unfair_lock_unlock(stripe_lock);
            return (0);
        }
        curr = curr->next;
    }
    os_unfair_lock_unlock(stripe_lock);
    return (ENOENT);
}

/**
 * @brief Increments the internal reference count of a cache instance,
 *        ensuring it is not deallocated while actively in use.
 * @details Atomically increments the cache's internal reference count to
 *          signal that a component or entry holds an active owner reference
 *          until a matching cache_release is performed.
 * @param cache A valid cache instance to retain (ignored if NULL).
 */
CACHE_PRIVATE_API void
cache_retain(cache_t *cache)
{
    if (cache == NULL)
        return;

    atomic_fetch_add_explicit(&cache->ref_count, 1, memory_order_relaxed);
}

/**
 * @brief Decrements the internal reference count of a cache instance and
 *        cleans up the cache when the reference count drops to zero.
 * @details Atomically decrements the cache's internal reference count. When
 *          the reference count drops to 0, cache_release cancels memory
 *          pressure monitoring, invokes cache_remove_all_locked to evict all
 *          live entries, frees associated resources, and deallocates the
 *          cache container.
 * @param cache A valid cache instance to release (ignored if NULL).
 */
CACHE_PRIVATE_API void
cache_release(cache_t *cache)
{
    uint32_t prev_ref;

    if (cache == NULL)
        return;

    prev_ref = atomic_fetch_sub_explicit(&cache->ref_count, 1,
        memory_order_acq_rel);
    if (prev_ref > 1)
        return;

    /* Signal teardown, cancel sources, and drain the serial monitor queue */
    atomic_store_explicit(&cache->is_destroying, true, memory_order_release);
    _cache_sim_pressure_unenroll(cache);

    if (cache->mem_pressure_source != NULL) {
        dispatch_source_cancel(cache->mem_pressure_source);
    }
    if (cache->sim_pressure_source != NULL) {
        dispatch_source_cancel(cache->sim_pressure_source);
    }
    if (cache->monitor_queue != NULL) {
        dispatch_sync(cache->monitor_queue, ^{
            if (cache->mem_pressure_source != NULL) {
                dispatch_release(cache->mem_pressure_source);
                cache->mem_pressure_source = NULL;
            }
            if (cache->sim_pressure_source != NULL) {
                dispatch_release(cache->sim_pressure_source);
                cache->sim_pressure_source = NULL;
            }
        });
        dispatch_release(cache->monitor_queue);
        cache->monitor_queue = NULL;
    }

    /* Acquire all locks and invoke cache_remove_all_locked to evict all live entries */
    cache_lock_stripes(cache);
    cache_lock_lru(cache);

    cache_remove_all_locked(cache);

    cache_unlock_lru(cache);
    cache_unlock_stripes(cache);

    /* Free internal hash table, lock array, name string, and cache container */
    free(cache->buckets);
    free(cache->locks);
    free(cache->name);
    free(cache);
}

/**
 * @brief Forcefully invalidates and tears down the cache instance
 *        immediately.
 * @details Clears any remaining entry retain counts and resets the internal
 *          reference count balance to 1 before calling cache_release()
 *          directly to evict all entries and deallocate the cache container.
 * @param cache A valid non-NULL cache instance to destroy.
 * @return 0 on success, or EINVAL if cache is NULL.
 */
CACHE_PUBLIC_API int
cache_destroy(cache_t *cache)
{
    cache_entry_t *curr;
    size_t b;

    if (cache == NULL)
        return (EINVAL);

    atomic_store_explicit(&cache->is_destroying, true, memory_order_release);

    cache_lock_stripes(cache);
    cache_lock_lru(cache);

    for (curr = cache->prot_head; curr != NULL; curr = curr->lru_next)
        curr->retain_count = 0;

    for (curr = cache->prob_head; curr != NULL; curr = curr->lru_next)
        curr->retain_count = 0;

    for (b = 0; b < cache->bucket_count; b++) {
        for (curr = cache->buckets[b].val_head; curr != NULL; curr = curr->val_next)
            curr->retain_count = 0;
    }

    atomic_store_explicit(&cache->ref_count, 1, memory_order_release);

    cache_unlock_lru(cache);
    cache_unlock_stripes(cache);

    cache_release(cache);
    return (0);
}

/**
 * @brief Sets the minimum number of values hint on the cache instance.
 * @param cache A valid cache instance.
 * @param hint Minimum number of values hint to store.
 * @return 0 on success, or EINVAL if cache is NULL.
 */
CACHE_PRIVATE_API int
cache_set_minimum_values_hint(cache_t *cache, size_t hint)
{
    if (cache == NULL)
        return (EINVAL);
    atomic_store_explicit(&cache->minimum_values_hint, hint,
        memory_order_relaxed);
    return (0);
}

/**
 * @brief Sets the maximum entry count hint ceiling on the cache instance.
 * @param cache A valid cache instance.
 * @param count_hint Maximum number of entries permitted before LRU tail
 *                   eviction is triggered.
 * @return 0 on success, or EINVAL if cache is NULL.
 */
CACHE_PRIVATE_API int
cache_set_count_hint(cache_t *cache, size_t count_hint)
{
    if (cache == NULL)
        return (EINVAL);
    atomic_store_explicit(&cache->configured_count_hint, count_hint,
        memory_order_relaxed);
    return (0);
}

/**
 * @brief Updates the cost value associated with an existing cached key and
 *        adjusts the cache's total cost.
 * @param cache A valid cache instance.
 * @param key Non-NULL key identifying the entry to update.
 * @param cost New cost value to assign to the entry.
 * @return 0 on success, EINVAL if cache or key is NULL, or ENOENT if the key
 *         is not found.
 */
CACHE_PRIVATE_API int
cache_set_cost_hint(cache_t *cache, void *key, cache_cost_t cost)
{
    cache_entry_t *curr;
    os_unfair_lock_t stripe_lock;
    cache_cost_t old_cost;
    uintptr_t hash_val;

    if (cache == NULL || key == NULL)
        return (EINVAL);

    hash_val = CACHE_KEY_HASH(cache, key);
    stripe_lock = GET_STRIPE_LOCK(cache, hash_val);

    os_unfair_lock_lock(stripe_lock);
    curr = GET_KEY_BUCKET(cache, hash_val);

    while (curr != NULL) {
        if (CACHE_KEY_MATCH(cache, curr, key, hash_val)) {
            old_cost = curr->cost;
            curr->cost = cost;

            /* Adjust the cache's aggregate cost by the delta */
            if (cost > old_cost) {
                atomic_fetch_add_explicit(&cache->current_cost,
                    (cost - old_cost), memory_order_relaxed);
            } else if (cost < old_cost) {
                atomic_fetch_sub_explicit(&cache->current_cost,
                    (old_cost - cost), memory_order_relaxed);
            }

            os_unfair_lock_unlock(stripe_lock);
            return (0);
        }
        curr = curr->next;
    }

    os_unfair_lock_unlock(stripe_lock);
    return (ENOENT);
}

/**
 * @brief Updates the descriptive name label assigned to the cache instance.
 * @param cache A valid cache instance.
 * @param name Non-NULL null-terminated string to duplicate and assign as the
 *             cache name.
 * @return 0 on success, EINVAL if cache or name is NULL, or ENOMEM if memory
 *         allocation fails.
 */
CACHE_PRIVATE_API int
cache_set_name(cache_t *cache, const char *name)
{
    char *new_name, *old_name;

    if (cache == NULL || name == NULL)
        return (EINVAL);

    new_name = strdup(name);
    if (new_name == NULL)
        return (ENOMEM);

    cache_lock_stripes(cache);

    old_name = cache->name;
    cache->name = new_name;

    cache_unlock_stripes(cache);

    free(old_name);
    return (0);
}

/**
 * @brief Invokes a callback for a key, retaining an existing entry during
 *        execution or inserting a newly created value on a miss.
 * @details On a cache hit, the entry is retained and promoted to the LRU
 *          head, invoke_cb is called outside the lock with exists=true, and
 *          the retain hold is then released. On a cache miss, invoke_cb is
 *          called with exists=false; if it outputs a non-NULL value, the
 *          value is inserted into the cache with cost 0 and unretained.
 * @param cache A valid cache instance.
 * @param key Non-NULL key identifier to query.
 * @param invoke_cb Callback function invoked outside the cache lock on hit
 *                  or miss.
 * @param context User-defined context pointer forwarded to invoke_cb.
 * @return 0 on a cache hit or when a new non-NULL value is produced and
 *         inserted, EINVAL if cache, key, or invoke_cb is NULL, or ENOENT if
 *         the key is missing and invoke_cb does not supply a value.
 */
CACHE_PRIVATE_API int
cache_invoke(cache_t *cache, void *key, cache_invoke_cb_t invoke_cb,
    void *context)
{
    cache_entry_t *curr;
    os_unfair_lock_t stripe_lock;
    void *allocated_value;
    uintptr_t hash_val;

    if (cache == NULL || key == NULL || invoke_cb == NULL)
        return (EINVAL);

    hash_val = CACHE_KEY_HASH(cache, key);
    stripe_lock = GET_STRIPE_LOCK(cache, hash_val);

    os_unfair_lock_lock(stripe_lock);
    curr = GET_KEY_BUCKET(cache, hash_val);

    while (curr != NULL) {
        if (CACHE_KEY_MATCH(cache, curr, key, hash_val)) {
            
            /* Retain the entry and promote it to the MRU head before unlocking */
            cache_lock_lru(cache);
            if (curr->value != NULL) {
                curr->retain_count++;
                cache_retain(cache);
            }
            touch_entry(cache, curr);
            cache_unlock_lru(cache);
            os_unfair_lock_unlock(stripe_lock);

            /* Invoke the callback outside the lock with the pinned entry */
            invoke_cb(curr->key, &curr->value, true, context);

            /* Release the temporary retain hold acquired for the callback */
            (void)cache_release_value(cache, curr->value);
            return (0);
        }
        curr = curr->next;
    }

    os_unfair_lock_unlock(stripe_lock);
    allocated_value = NULL;
    
    /* Invoke the callback on cache miss to optionally materialize a new value */
    invoke_cb(key, &allocated_value, false, context);

    if (allocated_value != NULL) {
        /* Insert materialized value with cost 0 and release initial retain hold */
        if (cache_set_and_retain(cache, key, allocated_value, 0) == 0)
            cache_release_value(cache, allocated_value);
        return (0);
    }

    return (ENOENT);
}
