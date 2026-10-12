/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#ifndef _CACHE_CALLBACKS_H_
#define _CACHE_CALLBACKS_H_

#include <stdbool.h>
#include <stdint.h>
#include <sys/cdefs.h>

#include <cache.h>

__BEGIN_DECLS

/**
 * @brief Computes a 64-bit FNV-1a hash from a null-terminated C string key.
 * @param key Null-terminated C string key pointer.
 * @param unused Unused user context parameter.
 * @return Computed hash value, or 0 if key is NULL or empty.
 */
CACHE_PUBLIC_API uintptr_t cache_key_hash_cb_cstring(void *key, void *unused);

/**
 * @brief Compares two null-terminated C string keys for equality.
 * @param key1 First C string key pointer.
 * @param key2 Second C string key pointer.
 * @param unused Unused user context parameter.
 * @return True if both keys are non-NULL and equal, false otherwise.
 */
CACHE_PUBLIC_API bool cache_key_is_equal_cb_cstring(void *key1, void *key2,
    void *unused);

/**
 * @brief Computes a 64-bit FNV-1a hash from an integer or pointer key.
 * @param key Integer value or pointer encoded as void *.
 * @param unused Unused user context parameter.
 * @return Computed hash value of the key.
 */
CACHE_PUBLIC_API uintptr_t cache_key_hash_cb_integer(void *key, void *unused);

/**
 * @brief Compares two integer or pointer keys for direct value equality.
 * @param key1 First integer or pointer key.
 * @param key2 Second integer or pointer key.
 * @param unused Unused user context parameter.
 * @return True if key1 == key2, false otherwise.
 */
CACHE_PUBLIC_API bool cache_key_is_equal_cb_integer(void *key1, void *key2,
    void *unused);

/**
 * @brief Computes a 64-bit FNV-1a hash over a fixed-length byte buffer.
 * @param data Pointer to the byte buffer to hash.
 * @param bytes Number of bytes to hash.
 * @return Computed hash value, or 0 if data is NULL or bytes is 0.
 */
CACHE_PUBLIC_API uintptr_t cache_hash_byte_string(const char *data,
    size_t bytes);

/**
 * @brief Marks a VM-allocated value region as volatile (purgeable).
 * @param value Pointer to a Mach VM purgeable memory region.
 * @param unused Unused user context parameter.
 */
CACHE_PUBLIC_API void cache_value_make_purgeable_cb(void *value,
    void *unused);

/**
 * @brief Marks a VM-allocated value region as non-volatile (non-purgeable).
 * @param value Pointer to a Mach VM purgeable memory region.
 * @param unused Unused user context parameter.
 * @return False if the region was purged by the kernel, true otherwise.
 */
CACHE_PUBLIC_API bool cache_value_make_nonpurgeable_cb(void *value,
    void *unused);

/**
 * @brief Releases a heap-allocated key or value via free(3).
 * @param key_or_value Heap-allocated pointer to free.
 * @param unused Unused user context parameter.
 */
CACHE_PUBLIC_API void cache_release_cb_free(void *key_or_value, void *unused);

__END_DECLS

#endif /* _CACHE_CALLBACKS_H_ */
