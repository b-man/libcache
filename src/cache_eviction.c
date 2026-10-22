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
static void
_cache_sim_pressure_enroll(cache_t *cache)
{
    if (cache != NULL)
        _cache_sim_pressure(cache, true, 0);
}

/**
 * @brief Unenrolls a cache instance from simulated memory pressure events.
 * @param cache Target cache instance to unenroll.
 */
static void
_cache_sim_pressure_unenroll(cache_t *cache)
{
    if (cache != NULL)
        _cache_sim_pressure(cache, false, 0);
}

/**
 * @brief GCD event callback for kernel memory-pressure notifications.
 * @details Reads memory-pressure flags from the kernel dispatch source and
 *          triggers cache eviction and capacity scaling when active.
 * @param context Opaque pointer to the target cache instance.
 */
static void
_cache_mem_pressure_handler(void *context)
{
    cache_t *cache = (cache_t *)context;
    unsigned long raw_flags;
    dispatch_source_memorypressure_flags_t flags;

    if (cache == NULL || atomic_load_explicit(&cache->is_destroying, memory_order_acquire))
        return;

    raw_flags = dispatch_source_get_data(cache->mem_pressure_source);
    flags = (dispatch_source_memorypressure_flags_t)raw_flags;

    if (flags & (DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL | DISPATCH_MEMORYPRESSURE_NORMAL)) {
        _cache_handle_memory_pressure_event(cache, flags);
    }
}

/**
 * @brief GCD event callback for simulated memory-pressure notifications.
 * @details Reads merged synthetic pressure flags from the data-or dispatch
 *          source and triggers cache eviction when active.
 * @param context Opaque pointer to the target cache instance.
 */
static void
_cache_sim_pressure_handler(void *context)
{
    cache_t *cache = (cache_t *)context;
    unsigned long raw_flags;
    dispatch_source_memorypressure_flags_t flags;

    if (cache == NULL || atomic_load_explicit(&cache->is_destroying, memory_order_acquire))
        return;

    raw_flags = dispatch_source_get_data(cache->sim_pressure_source);
    flags = (dispatch_source_memorypressure_flags_t)raw_flags;

    if (flags & (DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL | DISPATCH_MEMORYPRESSURE_NORMAL)) {
        _cache_handle_memory_pressure_event(cache, flags);
    }
}

/**
 * @brief Initializes and activates memory pressure event sources and monitoring queue.
 * @details Creates a serial monitor queue targeting the low-priority global queue,
 *          configures GCD memory-pressure and simulated event sources using pure C
 *          function callbacks to avoid block-retain cycles, and enrolls the cache
 *          for pressure notifications.
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
    cache->monitor_queue = dispatch_queue_create(CACHE_MONITOR_QUEUE_LABEL,
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

        if (cache->mem_pressure_source != NULL) {
            dispatch_set_context(cache->mem_pressure_source, cache);
            dispatch_source_set_event_handler_f(cache->mem_pressure_source,
                _cache_mem_pressure_handler);
            dispatch_resume(cache->mem_pressure_source);
        }
        if (cache->sim_pressure_source != NULL) {
            dispatch_set_context(cache->sim_pressure_source, cache);
            dispatch_source_set_event_handler_f(cache->sim_pressure_source,
                _cache_sim_pressure_handler);
            dispatch_resume(cache->sim_pressure_source);
        }
        _cache_sim_pressure_enroll(cache);
    }
}

/**
 * @brief No-op callback executed synchronously to drain the monitor queue.
 * @details Used with dispatch_sync_f during cache teardown to act as a barrier,
 *          ensuring any in-flight memory-pressure event handlers on the serial
 *          monitor queue complete execution before the queue is released.
 * @param context Unused callback context pointer.
 */
static void
_cache_noop_drain_cb(void *context)
{
    (void)context;
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

    /* Signal teardown and unenroll from simulation registry */
    atomic_store_explicit(&cache->is_destroying, true, memory_order_release);
    _cache_sim_pressure_unenroll(cache);

    /* Cancel and release memory-pressure event source */
    if (cache->mem_pressure_source != NULL) {
        dispatch_source_cancel(cache->mem_pressure_source);
        dispatch_release(cache->mem_pressure_source);
        cache->mem_pressure_source = NULL;
    }

    /* Cancel and release simulated pressure event source */
    if (cache->sim_pressure_source != NULL) {
        dispatch_source_cancel(cache->sim_pressure_source);
        dispatch_release(cache->sim_pressure_source);
        cache->sim_pressure_source = NULL;
    }

    /* Synchronously drain in-flight event handlers on monitor queue before releasing */
    if (cache->monitor_queue != NULL) {
        dispatch_sync_f(cache->monitor_queue, NULL, _cache_noop_drain_cb);
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
