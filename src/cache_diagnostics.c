/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>

#include "cache.h"
#include "cache_internal.h"

/**
 * @brief Retrieves the cost value currently assigned to a specific key.
 * @param cache A valid cache instance.
 * @param key Non-NULL key to look up.
 * @param cost_out Output pointer where the entry's cost value is written.
 * @return 0 on success, EINVAL if cache, key, or cost_out is NULL, or ENOENT
 *         if the key is not found.
 */
CACHE_PRIVATE_API int
cache_get_cost_hint(cache_t *cache, void *key, cache_cost_t *cost_out)
{
    cache_entry_t *curr;
    os_unfair_lock_t stripe_lock;
    uintptr_t hash_val;

    if (cache == NULL || key == NULL || cost_out == NULL)
        return (EINVAL);

    hash_val = CACHE_KEY_HASH(cache, key);
    stripe_lock = GET_STRIPE_LOCK(cache, hash_val);

    os_unfair_lock_lock(stripe_lock);
    curr = GET_KEY_BUCKET(cache, hash_val);
    
    /* Search the bucket chain for a matching key */
    while (curr != NULL) {
        if (CACHE_KEY_MATCH(cache, curr, key, hash_val)) {
            
            /* Write the entry's cost to the output pointer */
            *cost_out = curr->cost;
            os_unfair_lock_unlock(stripe_lock);
            return (0);
        }
        curr = curr->next;
    }
    os_unfair_lock_unlock(stripe_lock);
    return (ENOENT);
}

/**
 * @brief Retrieves the configured maximum entry count hint for the cache.
 * @param cache A valid cache instance.
 * @param count_out Output pointer where the configured entry count ceiling
 *                  is written.
 * @return 0 on success, or EINVAL if cache or count_out is NULL.
 */
CACHE_PRIVATE_API int
cache_get_count_hint(cache_t *cache, size_t *count_out)
{
    if (cache == NULL || count_out == NULL)
        return (EINVAL);

    *count_out = atomic_load_explicit(&cache->configured_count_hint,
        memory_order_relaxed);
    return (0);
}

/**
 * @brief Retrieves the configured minimum values hint for the cache.
 * @param cache A valid cache instance.
 * @param hint_out Output pointer where the minimum values hint is written.
 * @return 0 on success, or EINVAL if cache or hint_out is NULL.
 */
CACHE_PRIVATE_API int
cache_get_minimum_values_hint(cache_t *cache, size_t *hint_out)
{
    if (cache == NULL || hint_out == NULL)
        return (EINVAL);

    *hint_out = atomic_load_explicit(&cache->minimum_values_hint,
        memory_order_relaxed);
    return (0);
}

/**
 * @brief Retrieves an aggregate snapshot of the cache's current total cost
 *        and entry count.
 * @param cache A valid cache instance.
 * @param info_out Output structure populated with current_cost and
 *                 current_count.
 * @return 0 on success, or EINVAL if cache or info_out is NULL.
 */
CACHE_PRIVATE_API int
cache_get_info(cache_t *cache, cache_info_t *info_out)
{
    if (cache == NULL || info_out == NULL)
        return (EINVAL);

    info_out->current_cost = atomic_load_explicit(&cache->current_cost,
        memory_order_relaxed);
    info_out->current_count = atomic_load_explicit(&cache->current_count,
        memory_order_relaxed);
    return (0);
}

/**
 * @brief Retrieves the cost and retain count for a single key in the cache.
 * @param cache A valid cache instance.
 * @param key Non-NULL key to look up.
 * @param info_out Output structure populated with the entry's cost and
 *                 retain_count.
 * @return 0 on success, EINVAL if cache, key, or info_out is NULL, or ENOENT
 *         if the key is not found.
 */
CACHE_PRIVATE_API int
cache_get_info_for_key(cache_t *cache, void *key, cache_entry_info_t *info_out)
{
    cache_entry_t *curr;
    os_unfair_lock_t stripe_lock;
    uintptr_t hash_val;

    if (cache == NULL || key == NULL || info_out == NULL)
        return (EINVAL);

    hash_val = CACHE_KEY_HASH(cache, key);
    stripe_lock = GET_STRIPE_LOCK(cache, hash_val);

    os_unfair_lock_lock(stripe_lock);
    curr = GET_KEY_BUCKET(cache, hash_val);
    
    /* Search the bucket chain for a matching key */
    while (curr != NULL) {
        if (CACHE_KEY_MATCH(cache, curr, key, hash_val)) {
            info_out->cost = curr->cost;
            info_out->retain_count = curr->retain_count;
            os_unfair_lock_unlock(stripe_lock);
            return (0);
        }
        curr = curr->next;
    }
    os_unfair_lock_unlock(stripe_lock);
    return (ENOENT);
}

/**
 * @brief Retrieves cost and retain count metadata for an array of keys in a
 *        batch operation.
 * @details Populates info_out for each key. If a non-NULL key is not found
 *          in the cache, its corresponding info_out[i].retain_count is set
 *          to -1. NULL key elements are skipped with info_out[i]
 *          zero-initialized.
 * @param cache A valid cache instance.
 * @param keys Array of key pointers to query.
 * @param info_out Output array of cache_entry_info_t structures of length
 *                 count.
 * @param count Number of elements in the keys and info_out arrays (must be
 *              greater than 0).
 * @return 0 if all count keys were found, the number of matching keys found
 *         if fewer than count were matched, or EINVAL if cache, keys, or
 *         info_out is NULL or count is 0.
 */
CACHE_PRIVATE_API int
cache_get_info_for_keys(cache_t *cache, void **keys, cache_entry_info_t *info_out,
    size_t count)
{
    os_unfair_lock_t stripe_lock;
    cache_entry_t *curr;
    void *key;
    uintptr_t hash_val;
    size_t i;
    int total_found;
    bool found;

    if (cache == NULL || keys == NULL || info_out == NULL || count == 0)
        return (EINVAL);

    total_found = 0;
    
    /* Query each key in the input array */
    for (i = 0; i < count; i++) {
        key = keys[i];
        memset(&info_out[i], 0, sizeof(cache_entry_info_t));
        if (key == NULL)
            continue;

        hash_val = CACHE_KEY_HASH(cache, key);
        stripe_lock = GET_STRIPE_LOCK(cache, hash_val);

        os_unfair_lock_lock(stripe_lock);
        curr = GET_KEY_BUCKET(cache, hash_val);
        found = false;
        
        while (curr != NULL) {
            if (CACHE_KEY_MATCH(cache, curr, key, hash_val)) {
                info_out[i].cost = curr->cost;
                info_out[i].retain_count = curr->retain_count;
                found = true;
                total_found++;
                break;
            }
            curr = curr->next;
        }
        os_unfair_lock_unlock(stripe_lock);
        
        /* Mark missing non-NULL keys with a retain_count of -1 */
        if (!found)
            info_out[i].retain_count = -1;
    }
    return (total_found == (int)count ? 0 : total_found);
}

/**
 * @brief Returns the descriptive name string associated with the cache
 *        instance.
 * @param cache A valid cache instance, or NULL.
 * @return Pointer to the cache's internal name string, or NULL if cache is
 *         NULL.
 */
CACHE_PRIVATE_API const char *
cache_get_name(cache_t *cache)
{
    return (cache ? cache->name : NULL);
}

/**
 * @brief Prints all active entries in LRU order (from most-recently-used to
 *        least-recently-used) to standard output.
 * @param cache A valid cache instance to inspect (ignored if NULL).
 */
CACHE_PRIVATE_API void
cache_print(cache_t *cache)
{
    cache_entry_t *curr;

    if (cache == NULL)
        return;

    cache_lock_stripes(cache);
    cache_lock_lru(cache);

    printf("--- Cache Snapshot Dump: %s ---\n", cache->name);
    curr = cache->head;

    while (curr != NULL) {
        printf("  Key=%p | Cost=%zu | Holds=%d\n", curr->key,
            curr->cost, curr->retain_count);
        curr = curr->lru_next;
    }

    cache_unlock_lru(cache);
    cache_unlock_stripes(cache);
}

/**
 * @brief Prints a summary of the cache's runtime metrics and hash table
 *        configuration to standard output.
 * @details Displays the cache name, live entry count, aggregate cost, stripe
 *          count, bucket count, bucket mask, and count hint ceiling.
 * @param cache A valid cache instance to inspect (ignored if NULL).
 */
CACHE_PRIVATE_API void
cache_print_stats(cache_t *cache)
{
    if (cache == NULL)
        return;

    cache_lock_stripes(cache);

    printf("--- Cache Runtime Detailed Metrics Snapshot ---\n");
    printf("  Label Descriptor    : %s\n", cache->name);
    printf("  Live Node Count     : %zu\n", atomic_load(&cache->current_count));
    printf("  Aggregated cost Sum : %zu\n", atomic_load(&cache->current_cost));
    printf("  Lock Stripe Shards  : %zu concurrent lanes\n", cache->num_stripes);
    printf("  Allocated Buckets   : %zu tables slots\n", cache->bucket_count);
    printf("  Bitwise Mask Value  : 0x%zx\n", cache->bucket_mask);
    printf("  Count Hint Boundary : %zu nodes max ceiling\n", atomic_load(&cache->configured_count_hint));
    printf("-----------------------------------------------\n");

    cache_unlock_stripes(cache);
}
