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
unlink_entry(cache_t *cache, cache_entry_t *entry)
{
    cache_entry_t *curr, *prev;

    if (cache == NULL || entry == NULL)
        return;

    /* Unlink from the key hash bucket chain */
    curr = GET_KEY_BUCKET(cache, entry->key_hash);
    prev = NULL;
    while (curr != NULL) {
        if (curr == entry) {
            if (prev != NULL)
                prev->next = curr->next;
            else
                GET_KEY_BUCKET(cache, entry->key_hash) = curr->next;
            curr->next = NULL;
            break;
        }
        prev = curr;
        curr = curr->next;
    }

    /* Unlink from the doubly linked LRU queue */
    detach_lru(cache, entry);

    /* Unlink from the value-pointer hash bucket chain */
    detach_val_bucket(cache, entry);

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
    if (cache->attrs.value_release_cb != NULL) {
        cache->attrs.value_release_cb(entry->value, cache->attrs.user_data);
    }

    if (cache->attrs.key_release_cb != NULL) {
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
int
_cache_handle_memory_pressure_event(cache_t *cache, dispatch_source_memorypressure_flags_t flags)
{
    cache_entry_t *curr, *next_entry;
    size_t b, active_capacity;

    if (cache == NULL)
        return (EINVAL);

    cache_lock_stripes(cache);
    cache_lock_lru(cache);

    active_capacity = cache->bucket_count;

    /* Scan all key buckets for unretained or purgeable entries */
    for (b = 0; b < active_capacity; b++) {
        curr = cache->buckets[b];

        while (curr != NULL) {
            next_entry = curr->next;

            /* Evict unretained entries under warning or critical memory pressure */
            if (curr->retain_count == 0) {
                if (flags & (DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL)) {
                    unlink_entry(cache, curr);
                    free_entry_resources(cache, curr);
                    curr = next_entry;
                    continue;
                }
            } else {
                /* Under critical pressure, mark retained values as purgeable if supported */
                if (flags & DISPATCH_MEMORYPRESSURE_CRITICAL) {
                    if (cache->attrs.value_make_purgeable_cb != NULL) {
                        cache->attrs.value_make_purgeable_cb(curr->value,
                            cache->attrs.user_data);
                    }
                }
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
