/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_purgable.h>

#include "cache_callbacks.h"

/* 64-bit FNV-1a offset basis and prime constants. */
#define FNV1A_64_OFFSET_BASIS 0xcbf29ce484222325ULL
#define FNV1A_64_PRIME        0x100000001b3ULL

/**
 * @brief Computes a 64-bit FNV-1a hash from a null-terminated C string key.
 * @param key Pointer to the null-terminated C string key to hash.
 * @param unused Unused user data context parameter.
 * @return The computed 64-bit FNV-1a hash value, or 0 if key is NULL or
 *         empty.
 */
CACHE_PUBLIC_API uintptr_t
cache_key_hash_cb_cstring(void *key, void *unused)
{
    (void)unused;
    uint64_t hash = FNV1A_64_OFFSET_BASIS;
    const uint8_t *str = (const uint8_t *)key;

    if (str == NULL || *str == '\0')
        return (0);

    while (*str) {
        hash ^= *str++;
        hash *= FNV1A_64_PRIME;
    }

    return ((uintptr_t)hash);
}

/**
 * @brief Compares two null-terminated C string keys for equality.
 * @param key1 Pointer to the first null-terminated C string key.
 * @param key2 Pointer to the second null-terminated C string key.
 * @param unused Unused user data context parameter.
 * @return True if both keys are non-NULL and contain identical strings,
 *         false otherwise.
 */
CACHE_PUBLIC_API bool
cache_key_is_equal_cb_cstring(void *key1, void *key2, void *unused)
{
    (void)unused;
    if (key1 == NULL || key2 == NULL)
        return (false);
    if (key1 == key2)
        return (true);
    return (strcmp((const char *)key1, (const char *)key2) == 0);
}

/**
 * @brief Computes a 64-bit FNV-1a hash from a raw integer or pointer key.
 * @param key Integer value or pointer encoded as a void * key.
 * @param unused Unused user data context parameter.
 * @return The computed 64-bit FNV-1a hash value of the key.
 */
CACHE_PUBLIC_API uintptr_t
cache_key_hash_cb_integer(void *key, void *unused)
{
    (void)unused;
    uint64_t hash = FNV1A_64_OFFSET_BASIS;
    const uint8_t *bytes = (const uint8_t *)&key;

    for (size_t i = 0; i < sizeof(key); i++) {
        hash ^= bytes[i];
        hash *= FNV1A_64_PRIME;
    }

    return ((uintptr_t)hash);
}

/**
 * @brief Compares two raw integer or pointer keys for direct value equality.
 * @param key1 First integer or pointer key value.
 * @param key2 Second integer or pointer key value.
 * @param unused Unused user data context parameter.
 * @return True if key1 and key2 are equal (key1 == key2), false otherwise.
 */
CACHE_PUBLIC_API bool
cache_key_is_equal_cb_integer(void *key1, void *key2, void *unused)
{
    (void)unused;
    return (key1 == key2);
}

/**
 * @brief Computes a 64-bit FNV-1a hash over a fixed-length byte buffer.
 * @param data Pointer to the raw byte buffer to hash.
 * @param bytes Number of bytes to hash from data.
 * @return The computed 64-bit FNV-1a hash value, or 0 if data is NULL or
 *         bytes is 0.
 */
CACHE_PUBLIC_API uintptr_t
cache_hash_byte_string(const char *data, size_t bytes)
{
    const unsigned char *buffer = (const unsigned char *)data;
    uint64_t hash = FNV1A_64_OFFSET_BASIS;
    size_t i = 0;

    if (buffer == NULL || bytes == 0)
        return (0);

    /* Unroll 4 bytes per iteration to reduce loop branch overhead */
    for (; i + 4 <= bytes; i += 4) {
        hash = (hash ^ (uint64_t)buffer[i]) * FNV1A_64_PRIME;
        hash = (hash ^ (uint64_t)buffer[i + 1]) * FNV1A_64_PRIME;
        hash = (hash ^ (uint64_t)buffer[i + 2]) * FNV1A_64_PRIME;
        hash = (hash ^ (uint64_t)buffer[i + 3]) * FNV1A_64_PRIME;
    }

    for (; i < bytes; i++) {
        hash ^= (uint64_t)buffer[i];
        hash *= FNV1A_64_PRIME;
    }

    return ((uintptr_t)hash);
}

/**
 * @brief Marks a value's backing Mach virtual memory pages as volatile
 *        (purgeable).
 * @details Invoked when a cached value's retain count drops to zero so the
 *          kernel can reclaim its pages under memory pressure.
 * @param value Pointer to a VM-allocated purgeable memory region.
 * @param unused Unused user data context parameter.
 */
CACHE_PUBLIC_API void
cache_value_make_purgeable_cb(void *value, void *unused)
{
    (void)unused;
    if (value != NULL) {
        int state = VM_PURGABLE_VOLATILE;
        mach_vm_purgable_control(mach_task_self(),
            (mach_vm_address_t)(uintptr_t)value,
            VM_PURGABLE_SET_STATE, &state);
    }
}

/**
 * @brief Marks a value's backing Mach virtual memory pages as non-volatile
 *        (non-purgeable) and checks whether they were purged.
 * @details Invoked when an unretained cached value is retained again to
 *          ensure its backing memory is still intact before returning it to
 *          the caller.
 * @param value Pointer to a VM-allocated purgeable memory region.
 * @param unused Unused user data context parameter.
 * @return False if the memory region was purged by the kernel
 *         (VM_PURGABLE_EMPTY), or true otherwise.
 */
CACHE_PUBLIC_API bool
cache_value_make_nonpurgeable_cb(void *value, void *unused)
{
    int state;
    kern_return_t kr;

    (void)unused;
    if (value == NULL)
        return (true);

    state = VM_PURGABLE_NONVOLATILE;
    kr = mach_vm_purgable_control(mach_task_self(),
        (mach_vm_address_t)(uintptr_t)value,
        VM_PURGABLE_SET_STATE, &state);
    if (kr == KERN_SUCCESS &&
        (state & VM_PURGABLE_STATE_MASK) == VM_PURGABLE_EMPTY) {
        return (false);
    }

    return (true);
}

/**
 * @brief Releases a heap-allocated key or value by passing it to free(3).
 * @param key_or_value Pointer to the heap-allocated key or value to free.
 * @param unused Unused user data context parameter.
 */
CACHE_PUBLIC_API void
cache_release_cb_free(void *key_or_value, void *unused)
{
    (void)unused;
    free(key_or_value);
}
