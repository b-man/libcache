/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#include <stdlib.h>
#include <stdatomic.h>
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
 * @brief Promotes an accessed entry according to Segmented LRU (SLRU) rules.
 * @details If the entry is currently in the probationary segment, it is promoted
 *          to the protected segment MRU head. If it is already in the protected
 *          segment, it is moved to the MRU head of the protected segment. If
 *          the protected segment exceeds its capacity ceiling, its LRU tail entry
 *          is demoted back to the MRU head of the probationary segment.
 *          Caller must hold cache->lru_lock.
 * @param cache A valid cache instance.
 * @param entry Entry node that was accessed on a cache hit.
 */
void
touch_entry(cache_t *cache, cache_entry_t *entry)
{
    size_t count_limit, prot_limit;
    cache_entry_t *demote_node;

    if (cache == NULL || entry == NULL || entry->is_removed)
        return;

    if (entry->is_protected) {
        /* Fast path: entry is already at the protected MRU head */
        if (cache->prot_head == entry)
            return;

        /* Already protected: move to MRU head without changing prot_count */
        detach(cache, entry);
        attach_protected_head(cache, entry);
        return;
    }

    /* In probationary segment: promote to protected segment */
    detach(cache, entry);
    attach_protected_head(cache, entry);

    /* Enforce protected segment capacity only upon probationary promotion */
    count_limit = cache_get_effective_count_limit(cache);
    if (count_limit > 0) {
        prot_limit = (count_limit * 4) / 5;
        if (prot_limit == 0)
            prot_limit = 1;
        else if (prot_limit >= count_limit)
            prot_limit = count_limit - 1;

        /* Demote excess entries from protected tail to probationary head */
        while (cache->prot_count > prot_limit && cache->prot_tail != NULL) {
            demote_node = cache->prot_tail;
            detach(cache, demote_node);
            attach_probation_head(cache, demote_node);
        }
    }
}
