/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#ifndef _CACHE_H_
#define _CACHE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/cdefs.h>

/* Attribute structure version constants. */
#define CACHE_ATTRIBUTES_VERSION_1 1
#define CACHE_ATTRIBUTES_VERSION_2 2

#if defined(__GNUC__) || defined(__clang__)
    #define CACHE_PUBLIC_API __attribute__((visibility("default")))
#else
    #define CACHE_PUBLIC_API
#endif

__BEGIN_DECLS

/**
 * @brief Cost weight type associated with a cache entry.
 */
typedef size_t cache_cost_t;

/**
 * @brief Computes a hash value for a cache key.
 * @param key Key pointer to hash.
 * @param user_data User context pointer from cache attributes.
 * @return Computed hash value for the key.
 */
typedef uintptr_t (*cache_key_hash_cb_t)(void *key, void *user_data);

/**
 * @brief Compares two cache keys for equality.
 * @param key1 First key pointer.
 * @param key2 Second key pointer.
 * @param user_data User context pointer from cache attributes.
 * @return True if the keys are equal, false otherwise.
 */
typedef bool (*cache_key_is_equal_cb_t)(void *key1, void *key2,
    void *user_data);

/**
 * @brief Retains or copies a key upon insertion into the cache.
 * @param key_in Incoming key pointer.
 * @param key_out Output pointer receiving the retained key.
 * @param user_data User context pointer from cache attributes.
 */
typedef void (*cache_key_retain_cb_t)(void *key_in, void **key_out,
    void *user_data);

/**
 * @brief Releases a retained key or value when removed from the cache.
 * @param key_or_value Key or value pointer to release.
 * @param user_data User context pointer from cache attributes.
 */
typedef void (*cache_release_cb_t)(void *key_or_value, void *user_data);

/**
 * @brief Marks a cached value as non-purgeable before use.
 * @param value Value pointer to make non-purgeable.
 * @param user_data User context pointer from cache attributes.
 * @return True if the value is intact, or false if it was purged.
 */
typedef bool (*cache_value_make_nonpurgeable_cb_t)(void *value,
    void *user_data);

/**
 * @brief Marks an unretained cached value as purgeable.
 * @param value Value pointer to make purgeable.
 * @param user_data User context pointer from cache attributes.
 */
typedef void (*cache_value_make_purgeable_cb_t)(void *value, void *user_data);

/**
 * @brief Retains a value upon insertion into the cache (VERSION_2).
 * @param value_in Incoming value pointer to retain.
 * @param user_data User context pointer from cache attributes.
 */
typedef void (*cache_value_retain_cb_t)(void *value_in, void *user_data);

/**
 * @brief Configuration attributes and lifecycle callbacks for a cache.
 * @param version Structure version (CACHE_ATTRIBUTES_VERSION_1 or 2).
 * @param key_hash_cb Callback to compute key hashes (defaults to integer).
 * @param key_is_equal_cb Callback to test key equality (defaults to integer).
 * @param key_retain_cb Optional callback to retain or copy keys on insert.
 * @param key_release_cb Optional callback to release keys on removal.
 * @param value_release_cb Optional callback to release values on removal.
 * @param value_make_nonpurgeable_cb Optional callback to make values
 *                                   non-purgeable on retain.
 * @param value_make_purgeable_cb Optional callback to make values purgeable
 *                                when unretained.
 * @param user_data User context pointer passed to all callbacks.
 * @param value_retain_cb Optional callback to retain values on insert
 *                        (CACHE_ATTRIBUTES_VERSION_2 only).
 */
struct CACHE_PUBLIC_API cache_attributes_s {
    uint32_t version;
    cache_key_hash_cb_t key_hash_cb;
    cache_key_is_equal_cb_t key_is_equal_cb;
    cache_key_retain_cb_t key_retain_cb;
    cache_release_cb_t key_release_cb;
    cache_release_cb_t value_release_cb;
    cache_value_make_nonpurgeable_cb_t value_make_nonpurgeable_cb;
    cache_value_make_purgeable_cb_t value_make_purgeable_cb;
    void *user_data;
    cache_value_retain_cb_t value_retain_cb;
};

typedef struct cache_attributes_s cache_attributes_t;

/**
 * @brief Opaque handle to a cache instance.
 */
typedef struct cache_s cache_t;

/**
 * @brief Creates and initializes a new cache instance.
 * @param name Descriptive name for diagnostics.
 * @param attrs Configuration attributes and callbacks.
 * @param cache_out Output pointer receiving the created cache instance.
 * @return 0 on success, EINVAL on invalid arguments, or ENOMEM on failure.
 */
CACHE_PUBLIC_API int cache_create(const char *name, cache_attributes_t *attrs,
    cache_t **cache_out);

/**
 * @brief Inserts or updates a key-value entry and retains non-NULL values.
 * @param cache Target cache instance.
 * @param key Non-NULL key identifying the entry.
 * @param value Value pointer to store and retain, or NULL.
 * @param cost Cost weight assigned to the entry.
 * @return 0 on success, EINVAL on invalid arguments, or ENOMEM on failure.
 */
CACHE_PUBLIC_API int cache_set_and_retain(cache_t *cache, void *key,
    void *value, cache_cost_t cost);

/**
 * @brief Looks up a value by key and increments its retain count.
 * @param cache Target cache instance.
 * @param key Non-NULL key to look up.
 * @param value_out Output pointer receiving the retained value (or NULL).
 * @return 0 on success, EINVAL on invalid arguments, or ENOENT if not found.
 */
CACHE_PUBLIC_API int cache_get_and_retain(cache_t *cache, void *key,
    void **value_out);

/**
 * @brief Releases a previously retained reference to a cached value.
 * @param cache Target cache instance.
 * @param value Retained value pointer to release, or NULL.
 * @return 0 on success, EINVAL if cache is NULL, or ENOENT if not found.
 */
CACHE_PUBLIC_API int cache_release_value(cache_t *cache, void *value);

/**
 * @brief Destroys a cache instance and releases all remaining entries.
 * @param cache Valid cache instance to destroy.
 * @return 0 on success.
 */
CACHE_PUBLIC_API int cache_destroy(cache_t *cache);

/**
 * @brief Removes the entry associated with the specified key.
 * @param cache Target cache instance.
 * @param key Non-NULL key identifying the entry to remove.
 * @return 0 on success, EINVAL on invalid arguments, or ENOENT if not found.
 */
CACHE_PUBLIC_API int cache_remove(cache_t *cache, void *key);

/**
 * @brief Removes all entries from the cache.
 * @param cache Target cache instance.
 * @return 0 on success, or EINVAL if cache is NULL.
 */
CACHE_PUBLIC_API int cache_remove_all(cache_t *cache);

__END_DECLS

#endif /* _CACHE_H_ */
