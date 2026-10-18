/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#include <sys/types.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <errno.h>
#include <malloc/malloc.h>

#include "cache.h"
#include "cache_internal.h"

/**
 * @brief Unlinks a cache entry from the key hash table, value-pointer hash
 *        table, and LRU queue, and updates cache metrics.
 * @details Caller must hold the stripe lock covering entry->key_hash (or all
 *          stripe locks) and cache->lru_lock.
 * @param cache A valid cache instance.
 * @param entry Entry node to unlink from hash buckets and LRU queue.
 */
void
detach_entry(cache_t *cache, cache_entry_t *entry)
{
    if (cache == NULL || entry == NULL)
        return;

    /* Unlink from the key hash bucket chain */
    detach_key_bucket(cache, entry);

    /* Unlink from the value-pointer hash bucket chain */
    detach_val_bucket(cache, entry);

    /* Unlink from the doubly linked LRU queue */
    detach(cache, entry);

    /* Update aggregate cost and live entry count */
    atomic_fetch_sub_explicit(&cache->current_cost, entry->cost,
        memory_order_relaxed);
    atomic_fetch_sub_explicit(&cache->current_count, 1,
        memory_order_relaxed);
}

/**
 * @brief Marks an entry as removed from lookups while preserving it in the
 *        value-pointer index for deferred O(1) release.
 * @details Caller must hold both the entry's stripe lock and cache->lru_lock.
 * @param cache A valid cache instance.
 * @param entry Entry node to unlink from key lookups and LRU list.
 */
void
mark_entry_removed(cache_t *cache, cache_entry_t *entry)
{
    if (cache == NULL || entry == NULL)
        return;

    /* Unlink from key bucket so subsequent key lookups miss */
    detach_key_bucket(cache, entry);

    /* Unlink from the LRU queue so eviction ignores it */
    detach(cache, entry);

    /* Mark as removed; entry stays in value index until retain_count reaches 0 */
    entry->is_removed = true;

    /* Update aggregate cost and live entry count */
    atomic_fetch_sub_explicit(&cache->current_cost, entry->cost,
        memory_order_relaxed);
    atomic_fetch_sub_explicit(&cache->current_count, 1,
        memory_order_relaxed);
}

/**
 * @brief Invokes configured key and value release callbacks and frees the
 *        entry structure.
 * @param cache A valid cache instance.
 * @param entry Entry node whose resources and backing memory are to be
 *              released.
 */
void
free_entry_resources(cache_t *cache, cache_entry_t *entry)
{
    if (cache == NULL || entry == NULL)
        return;

    if (cache->attrs.value_release_cb != NULL && entry->value != NULL) {
        cache->attrs.value_release_cb(entry->value, cache->attrs.user_data);
    }

    if (cache->attrs.key_release_cb != NULL && entry->key != NULL) {
        cache->attrs.key_release_cb(entry->key, cache->attrs.user_data);
    }

    malloc_zone_free(cache->memory_zone, entry);
}

/**
 * @brief Handles system memory-pressure events by evicting unretained
 *        entries and marking retained values purgeable under critical
 *        pressure.
 * @param cache A valid cache instance.
 * @param flags Memory pressure bitmask flags (DISPATCH_MEMORYPRESSURE_WARN
 *              or DISPATCH_MEMORYPRESSURE_CRITICAL).
 * @return 0 on success, or EINVAL if cache is NULL.
 */
static int
_cache_handle_memory_pressure_event(cache_t *cache, dispatch_source_memorypressure_flags_t flags)
{
    dispatch_source_memorypressure_flags_t allowed_mask;
    cache_entry_t *curr, *next_entry;
    size_t b, active_capacity, limit;

    if (cache == NULL)
        return (EINVAL);

    allowed_mask = DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
                   DISPATCH_MEMORYPRESSURE_CRITICAL;

    if (flags == 0 || (flags & ~allowed_mask) != 0)
        return (EINVAL);

    /* Dynamic memory-proportional capacity adjustment under memory pressure */
    limit = cache->base_capacity_limit;
    if (flags & DISPATCH_MEMORYPRESSURE_CRITICAL) {
        limit = (limit / 4 < CACHE_MIN_CRITICAL_CAPACITY_LIMIT) ?
            CACHE_MIN_CRITICAL_CAPACITY_LIMIT : limit / 4;
    } else if (flags & DISPATCH_MEMORYPRESSURE_WARN) {
        limit = (limit / 2 < CACHE_MIN_WARN_CAPACITY_LIMIT) ?
            CACHE_MIN_WARN_CAPACITY_LIMIT : limit / 2;
    }
    atomic_store_explicit(&cache->dynamic_capacity_limit, limit,
        memory_order_relaxed);

    /* Only warning and critical memory pressure require scanning and evicting entries */
    if (!(flags & (DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL)))
        return (0);

    cache_lock_stripes(cache);
    cache_lock_lru(cache);

    active_capacity = cache->bucket_count;

    /* Scan all key buckets to evict unretained entries or mark values purgeable */
    for (b = 0; b < active_capacity; b++) {
        curr = cache->buckets[b].key_head;

        while (curr != NULL) {
            next_entry = curr->next;

            if (curr->retain_count == 0) {
                detach_entry(cache, curr);
                free_entry_resources(cache, curr);
            } else if ((flags & DISPATCH_MEMORYPRESSURE_CRITICAL) &&
                       cache->attrs.value_make_purgeable_cb != NULL) {
                cache->attrs.value_make_purgeable_cb(curr->value,
                    cache->attrs.user_data);
            }

            curr = next_entry;
        }
    }

    cache_unlock_lru(cache);
    cache_unlock_stripes(cache);

    return (0);
}

/**
 * @brief Enrolls, unenrolls, or signals active caches for simulated memory
 *        pressure events using function-scoped static state.
 * @param cache Target cache to enroll or unenroll, or NULL to broadcast flags.
 * @param enroll True to enroll cache, false to unenroll when cache is non-NULL.
 * @param flags Memory pressure flags to merge when cache is NULL.
 */
static void
_cache_sim_pressure(cache_t *cache, bool enroll,
    dispatch_source_memorypressure_flags_t flags)
{
    static os_unfair_lock sim_lock = OS_UNFAIR_LOCK_INIT;
    static cache_t *sim_head = NULL;
    cache_t *curr, *prev;

    os_unfair_lock_lock(&sim_lock);
    if (cache != NULL) {
        if (enroll) {
            cache->sim_next = sim_head;
            sim_head = cache;
        } else {
            curr = sim_head;
            prev = NULL;
            while (curr != NULL) {
                if (curr == cache) {
                    if (prev != NULL)
                        prev->sim_next = curr->sim_next;
                    else
                        sim_head = curr->sim_next;
                    curr->sim_next = NULL;
                    break;
                }
                prev = curr;
                curr = curr->sim_next;
            }
        }
    } else {
        for (curr = sim_head; curr != NULL; curr = curr->sim_next) {
            if (curr->sim_pressure_source != NULL)
                dispatch_source_merge_data(curr->sim_pressure_source, flags);
        }
    }
    os_unfair_lock_unlock(&sim_lock);
}

/**
 * @brief Enrolls a cache instance to receive simulated memory pressure events.
 * @param cache Target cache instance to enroll.
 */
void
_cache_sim_pressure_enroll(cache_t *cache)
{
    if (cache != NULL)
        _cache_sim_pressure(cache, true, 0);
}

/**
 * @brief Unenrolls a cache instance from simulated memory pressure events.
 * @param cache Target cache instance to unenroll.
 */
void
_cache_sim_pressure_unenroll(cache_t *cache)
{
    if (cache != NULL)
        _cache_sim_pressure(cache, false, 0);
}

/**
 * @brief Initializes and activates memory pressure event sources and monitoring queue.
 * @details Creates a serial monitor queue targeting the low-priority global queue,
 *          configures GCD memory-pressure and simulated event sources, and enrolls
 *          the cache for pressure notifications.
 * @param cache A valid cache instance pointer.
 */
void
cache_init_memory_pressure_monitor(cache_t *cache)
{
    dispatch_queue_t monitor_queue;

    if (cache == NULL)
        return;

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
}

/**
 * @brief Signals teardown, cancels memory-pressure event sources, and drains
 *        the serial monitor queue.
 * @param cache A valid cache instance.
 */
void
cache_teardown_memory_pressure_monitor(cache_t *cache)
{
    if (cache == NULL)
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
}

/**
 * @brief Broadcasts a simulated low-memory event via GCD to trigger cache
 *        memory-pressure handlers.
 * @param flags Non-zero bitmask of memory pressure flags.
 * @return 0 on success, or EINVAL if flags is 0 or contains unsupported bits.
 */
CACHE_PRIVATE_API int
cache_simulate_memory_warning_event(dispatch_source_memorypressure_flags_t flags)
{
    dispatch_source_memorypressure_flags_t allowed_mask;

    allowed_mask = DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
                   DISPATCH_MEMORYPRESSURE_CRITICAL;

    if (flags == 0 || (flags & ~allowed_mask) != 0)
        return (EINVAL);

    _cache_sim_pressure(NULL, false, flags);

    return (0);
}
