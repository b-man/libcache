/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#ifndef _CACHE_PRIVATE_H_
#define _CACHE_PRIVATE_H_

#include <sys/cdefs.h>
#include <dispatch/dispatch.h>
#include <dispatch/source.h>

#include <cache.h>

#if defined(CACHE_PRIVATE) && (defined(__GNUC__) || defined(__clang__))
    #define CACHE_PRIVATE_API __attribute__((visibility("default")))
#else
    #define CACHE_PRIVATE_API
#endif

__BEGIN_DECLS

/**
 * @brief Predicate block evaluated when conditionally removing entries.
 * @param key Key pointer of the unretained entry.
 * @param value Value pointer of the unretained entry.
 * @return True to remove the entry, false to keep it.
 */
typedef bool (^cache_remove_block_t)(void *key, void *value);

/**
 * @brief Callback function executed by cache_invoke when an entry is resolved.
 * @param value The data pointer associated with the key, safely retained for the callback duration.
 * @param context The user-defined context passed into cache_invoke.
 */
typedef void (*cache_invoke_cb_t)(void *value, void *context);

/**
 * @brief Aggregate cost and entry count snapshot of a cache.
 * @param current_cost Total cost across all active entries.
 * @param current_count Total number of active entries.
 */
struct CACHE_PRIVATE_API cache_info_s {
    cache_cost_t current_cost;
    size_t current_count;
};
typedef struct cache_info_s cache_info_t;

/**
 * @brief Metadata snapshot for an individual cache entry.
 * @param cost Cost weight assigned to the entry.
 * @param retain_count Current reference count (-1 if not found in batch).
 */
struct CACHE_PRIVATE_API cache_entry_info_s {
    cache_cost_t cost;
    int retain_count;
};
typedef struct cache_entry_info_s cache_entry_info_t;

/**
 * @brief Retrieves the cost value assigned to a specific key.
 * @param cache Target cache instance.
 * @param key Non-NULL key to look up.
 * @param cost_out Output pointer receiving the entry's cost.
 * @return 0 on success, EINVAL on invalid arguments, or ENOENT if not found.
 */
CACHE_PRIVATE_API int cache_get_cost_hint(cache_t *cache, void *key,
    cache_cost_t *cost_out);

/**
 * @brief Retrieves the configured maximum entry count hint.
 * @param cache Target cache instance.
 * @param count_out Output pointer receiving the count hint ceiling.
 * @return 0 on success, or EINVAL on invalid arguments.
 */
CACHE_PRIVATE_API int cache_get_count_hint(cache_t *cache, size_t *count_out);

/**
 * @brief Retrieves the dynamic memory-proportional capacity limit.
 * @param cache Target cache instance.
 * @param limit_out Output pointer receiving the dynamic capacity limit.
 * @return 0 on success, or EINVAL on invalid arguments.
 */
CACHE_PRIVATE_API int cache_get_dynamic_capacity_limit(cache_t *cache,
    size_t *limit_out);

/**
 * @brief Retrieves the configured minimum values hint.
 * @param cache Target cache instance.
 * @param hint_out Output pointer receiving the minimum values hint.
 * @return 0 on success, or EINVAL on invalid arguments.
 */
CACHE_PRIVATE_API int cache_get_minimum_values_hint(cache_t *cache,
    size_t *hint_out);

/**
 * @brief Retrieves a snapshot of the cache's total cost and entry count.
 * @param cache Target cache instance.
 * @param info_out Output structure populated with aggregate cost and count.
 * @return 0 on success, or EINVAL on invalid arguments.
 */
CACHE_PRIVATE_API int cache_get_info(cache_t *cache, cache_info_t *info_out);

/**
 * @brief Retrieves the cost and retain count for a single key.
 * @param cache Target cache instance.
 * @param key Non-NULL key to look up.
 * @param info_out Output structure populated with entry metadata.
 * @return 0 on success, EINVAL on invalid arguments, or ENOENT if not found.
 */
CACHE_PRIVATE_API int cache_get_info_for_key(cache_t *cache, void *key,
    cache_entry_info_t *info_out);

/**
 * @brief Retrieves cost and retain count metadata for an array of keys.
 * @param cache Target cache instance.
 * @param keys Array of key pointers to query.
 * @param info_out Output array of cache_entry_info_t structures.
 * @param count Number of elements in keys and info_out (must be > 0).
 * @return 0 if all keys were found, match count if partial, or EINVAL.
 */
CACHE_PRIVATE_API int cache_get_info_for_keys(cache_t *cache, void **keys,
    cache_entry_info_t *info_out, size_t count);

/**
 * @brief Returns the descriptive name associated with the cache instance.
 * @param cache Target cache instance, or NULL.
 * @return Pointer to the cache's name string, or NULL if cache is NULL.
 */
CACHE_PRIVATE_API const char *cache_get_name(cache_t *cache);

/**
 * @brief Prints all active entries in LRU order to standard output.
 * @param cache Target cache instance (ignored if NULL).
 */
CACHE_PRIVATE_API void cache_print(cache_t *cache);

/**
 * @brief Prints a summary of runtime metrics to standard output.
 * @param cache Target cache instance (ignored if NULL).
 */
CACHE_PRIVATE_API void cache_print_stats(cache_t *cache);

/**
 * @brief Sets the minimum number of values hint on the cache instance.
 * @param cache Target cache instance.
 * @param hint Minimum number of values hint to store.
 * @return 0 on success, or EINVAL if cache is NULL.
 */
CACHE_PRIVATE_API int cache_set_minimum_values_hint(cache_t *cache,
    size_t hint);

/**
 * @brief Sets the maximum entry count hint ceiling before LRU eviction.
 * @param cache Target cache instance.
 * @param count_hint Maximum entry count ceiling (0 disables count eviction).
 * @return 0 on success, or EINVAL if cache is NULL.
 */
CACHE_PRIVATE_API int cache_set_count_hint(cache_t *cache, size_t count_hint);

/**
 * @brief Updates the descriptive name assigned to the cache instance.
 * @param cache Target cache instance.
 * @param name Non-NULL string to duplicate and assign as the cache name.
 * @return 0 on success, EINVAL on invalid arguments, or ENOMEM on failure.
 */
CACHE_PRIVATE_API int cache_set_name(cache_t *cache, const char *name);

/**
 * @brief Removes unretained entries for which remove_block returns true.
 * @param cache Target cache instance.
 * @param remove_block Predicate block evaluated on each unretained entry.
 * @return Number of entries removed, or EINVAL on invalid arguments.
 */
CACHE_PRIVATE_API int cache_remove_with_block(cache_t *cache,
    cache_remove_block_t remove_block);

/**
 * @brief Updates the cost value of an existing key and adjusts total cost.
 * @param cache Target cache instance.
 * @param key Non-NULL key identifying the entry to update.
 * @param cost New cost weight to assign to the entry.
 * @return 0 on success, EINVAL on invalid arguments, or ENOENT if not found.
 */
CACHE_PRIVATE_API int cache_set_cost_hint(cache_t *cache, void *key,
    cache_cost_t cost);

/**
 * @brief Safely accesses and performs actions on a cached value within a protected scope.
 * @details Executes a user-provided callback function inside a protected scope where
 *          the value is guaranteed to remain valid and retained.
 * @param cache The target cache instance containing the data.
 * @param key A pointer to the unique identifier for the desired cache entry.
 * @param callback A function pointer executed when the entry is resolved.
 * @param context A pass-through pointer forwarded directly to callback.
 * @return 0 on success, or non-zero error code.
 */
CACHE_PRIVATE_API int cache_invoke(cache_t *cache, void *key,
    cache_invoke_cb_t callback, void *context);

/**
 * @brief Broadcasts a simulated low-memory warning event via GCD.
 * @param flags Non-zero memory pressure flags (WARN and/or CRITICAL).
 * @return 0 on success, or EINVAL if flags are invalid.
 */
CACHE_PRIVATE_API int cache_simulate_memory_warning_event(
    dispatch_source_memorypressure_flags_t flags);

/**
 * @brief Looks up a value by key without incrementing its retain count.
 * @param cache Target cache instance.
 * @param key Non-NULL key to query.
 * @param value_out Output pointer receiving the discovered value (or NULL).
 * @return 0 on success, EINVAL on invalid arguments, or ENOENT if not found.
 */
CACHE_PRIVATE_API int cache_get(cache_t *cache, void *key, void **value_out);

/**
 * @brief Increments the internal reference count of a cache instance,
 *        ensuring it is not deallocated while actively in use.
 * @param cache Target cache instance (ignored if NULL).
 */
CACHE_PRIVATE_API void cache_retain(cache_t *cache);

/**
 * @brief Decrements the internal reference count of a cache instance and
 *        cleans up and deallocates the cache when the reference count drops
 *        to zero.
 * @param cache Target cache instance (ignored if NULL).
 */
CACHE_PRIVATE_API void cache_release(cache_t *cache);

__END_DECLS

#endif /* _CACHE_PRIVATE_H_ */
