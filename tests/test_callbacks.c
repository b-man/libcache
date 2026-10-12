/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "cache.h"
#include "cache_callbacks.h"
#include "test_suite.h"

/*
 * Validates cache_key_hash_cb_cstring and cache_key_is_equal_cb_cstring
 * defined in cache_callbacks.h.
 */
void
test_cstring_key_callbacks(void)
{
    void *context = NULL;
    char *key1 = strdup("com.darwin.libcache.key.alpha");
    char *key2 = strdup("com.darwin.libcache.key.alpha");
    char *key3 = strdup("com.darwin.libcache.key.beta");
    char *empty_key = "";

    ASSERT_TRUE(key1 != NULL && key2 != NULL && key3 != NULL,
        "Test string allocations must succeed");

    /* Positive Path: Deterministic non-zero hashing for identical C-strings */
    uintptr_t hash1 = cache_key_hash_cb_cstring(key1, context);
    uintptr_t hash2 = cache_key_hash_cb_cstring(key2, context);
    ASSERT_TRUE(hash1 != 0,
        "C-string hash for a valid non-empty key must be non-zero");
    ASSERT_TRUE(hash1 == hash2,
        "Identical C-strings at distinct addresses must produce identical hashes");

    /* Positive Path: Distinct strings hash to distinct values */
    uintptr_t hash3 = cache_key_hash_cb_cstring(key3, context);
    ASSERT_TRUE(hash1 != hash3,
        "Distinct C-strings must produce distinct hashes");

    /* Boundary Path: Empty null-terminated string returns 0 */
    uintptr_t empty_hash = cache_key_hash_cb_cstring(empty_key, context);
    ASSERT_TRUE(empty_hash == 0,
        "Hashing an empty C-string must return 0");

    /* Positive Path: C-string equality checks */
    ASSERT_TRUE(cache_key_is_equal_cb_cstring(key1, key1, context) == true,
        "Comparing identical C-string pointer addresses must return true");
    ASSERT_TRUE(cache_key_is_equal_cb_cstring(key1, key2, context) == true,
        "Comparing distinct buffers with identical C-string contents must return true");
    ASSERT_TRUE(cache_key_is_equal_cb_cstring(key1, key3, context) == false,
        "Comparing different C-string contents must return false");

    /* Boundary Path: Empty C-string equality checks */
    ASSERT_TRUE(cache_key_is_equal_cb_cstring(key1, empty_key, context) == false,
        "Comparing a non-empty C-string against an empty C-string must return false");
    ASSERT_TRUE(cache_key_is_equal_cb_cstring(empty_key, key2, context) == false,
        "Comparing an empty C-string against a non-empty C-string must return false");
    ASSERT_TRUE(cache_key_is_equal_cb_cstring(empty_key, "", context) == true,
        "Comparing two empty C-strings must return true");

    free(key1);
    free(key2);
    free(key3);
}

/*
 * Validates cache_key_hash_cb_integer and cache_key_is_equal_cb_integer
 * defined in cache_callbacks.h.
 */
void
test_integer_key_callbacks(void)
{
    void *context = NULL;
    void *int_key1 = (void *)(uintptr_t)4096;
    void *int_key2 = (void *)(uintptr_t)4096;
    void *int_key3 = (void *)(uintptr_t)8192;

    /* Positive Path: Deterministic hashing for integer keys */
    uintptr_t hash1 = cache_key_hash_cb_integer(int_key1, context);
    uintptr_t hash2 = cache_key_hash_cb_integer(int_key2, context);
    uintptr_t hash3 = cache_key_hash_cb_integer(int_key3, context);

    ASSERT_TRUE(hash1 != 0,
        "Integer key hash for a non-zero key must be non-zero");
    ASSERT_TRUE(hash1 != (uintptr_t)int_key1,
        "Integer key hash must be a computed hash value rather than the raw key");
    ASSERT_TRUE(hash1 == hash2,
        "Equal integer keys must yield identical hash values");
    ASSERT_TRUE(hash1 != hash3,
        "Different integer keys must yield different hash values");

    /* Boundary Path: Zero/NULL integer key hashes deterministically */
    uintptr_t zero_hash1 = cache_key_hash_cb_integer(NULL, context);
    uintptr_t zero_hash2 = cache_key_hash_cb_integer((void *)(uintptr_t)0, context);
    ASSERT_TRUE(zero_hash1 != 0,
        "Zero/NULL integer key must produce a non-zero hash");
    ASSERT_TRUE(zero_hash1 == zero_hash2,
        "Zero/NULL integer key hash must be deterministic");
    ASSERT_TRUE(zero_hash1 != hash1,
        "Zero/NULL integer key hash must differ from non-zero key hash");

    /* Positive Path: Direct value equality comparisons */
    ASSERT_TRUE(cache_key_is_equal_cb_integer(int_key1, int_key2, context) == true,
        "Equal integer keys must compare equal");
    ASSERT_TRUE(cache_key_is_equal_cb_integer(int_key1, int_key3, context) == false,
        "Distinct integer keys must compare unequal");

    /* Boundary Path: NULL / zero integer key comparisons */
    ASSERT_TRUE(cache_key_is_equal_cb_integer(int_key1, NULL, context) == false,
        "Non-zero integer key and NULL must compare unequal");
    ASSERT_TRUE(cache_key_is_equal_cb_integer(NULL, int_key2, context) == false,
        "NULL and non-zero integer key must compare unequal");
    ASSERT_TRUE(cache_key_is_equal_cb_integer(NULL, NULL, context) == true,
        "Two zero/NULL integer keys must compare equal");
}

/*
 * Validates cache_hash_byte_string defined in cache_callbacks.h.
 */
void
test_hash_byte_string_callback(void)
{
    const char *buf1 = "libcache_byte_payload";
    const char *buf2 = "libcache_byte_payload!";
    const char binary_buf[] = { 'A', 'B', 'C', '\0', 'D', 'E', 'F' };
    size_t len1 = strlen(buf1);
    size_t len2 = strlen(buf2);

    /* Positive Path: Deterministic hashing over fixed-length byte buffers */
    uintptr_t hash1_a = cache_hash_byte_string(buf1, len1);
    uintptr_t hash1_b = cache_hash_byte_string(buf1, len1);
    uintptr_t hash2 = cache_hash_byte_string(buf2, len2);

    ASSERT_TRUE(hash1_a != 0,
        "Valid byte string hash must be non-zero");
    ASSERT_TRUE(hash1_a == hash1_b,
        "Hashing the same byte sequence and length must be deterministic");
    ASSERT_TRUE(hash1_a == cache_key_hash_cb_cstring((void *)buf1, NULL),
        "cache_hash_byte_string over strlen(s) must match cache_key_hash_cb_cstring(s, NULL)");
    ASSERT_TRUE(hash1_a != hash2,
        "Different byte sequences must yield distinct hashes");

    /* Positive Path: Processes full byte length past embedded NUL bytes */
    uintptr_t hash_prefix = cache_hash_byte_string(binary_buf, 3);
    uintptr_t hash_full = cache_hash_byte_string(binary_buf, sizeof(binary_buf));
    ASSERT_TRUE(hash_prefix != 0 && hash_full != 0,
        "Binary buffer hashes must be non-zero");
    ASSERT_TRUE(hash_prefix != hash_full,
        "Byte string hashing must continue past embedded NUL bytes up to the specified length");

    /* Boundary Path: Zero byte count returns 0 */
    ASSERT_TRUE(cache_hash_byte_string(buf1, 0) == 0,
        "Hashing a non-empty buffer with zero byte length must return 0");
    ASSERT_TRUE(cache_hash_byte_string("", 0) == 0,
        "Hashing an empty buffer with zero byte length must return 0");
}

/*
 * Validates cache_value_make_purgeable_cb, cache_value_make_nonpurgeable_cb,
 * and cache_release_cb_free defined in cache_callbacks.h.
 */
void
test_value_purgeable_and_release_callbacks(void)
{
    void *context = NULL;
    void *heap_value = malloc(128);
    void *purgeable_buf = malloc(4096);

    ASSERT_TRUE(heap_value != NULL && purgeable_buf != NULL,
        "Sample heap allocations must succeed");

    /* Positive Path: Default free callback releases key_or_value allocation cleanly */
    cache_release_cb_free(heap_value, context);

    /* Positive Path: Purgeable and nonpurgeable callbacks execute cleanly on valid memory */
    cache_value_make_purgeable_cb(purgeable_buf, context);
    ASSERT_TRUE(cache_value_make_nonpurgeable_cb(purgeable_buf, context) == true,
        "cache_value_make_nonpurgeable_cb on valid non-purged memory must return true");
    free(purgeable_buf);

    /* Boundary Path: NULL value pointers are handled safely */
    cache_release_cb_free(NULL, context);
    cache_value_make_purgeable_cb(NULL, context);
    ASSERT_TRUE(cache_value_make_nonpurgeable_cb(NULL, context) == true,
        "cache_value_make_nonpurgeable_cb(NULL, context) must return true");
}
