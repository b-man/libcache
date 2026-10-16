/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#ifndef _TEST_SUITE_H_
#define _TEST_SUITE_H_

#include <stdio.h>
#include <stdbool.h>

extern int tests_run;
extern int tests_failed;

#define ASSERT_TRUE(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "  [FAIL] Assertion failed: %s (%s:%d)\n", \
            message, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define RUN_TEST(test_func) do { \
    printf("[RUN] %s...\n", #test_func); \
    tests_run++; \
    int failed_before = tests_failed; \
    test_func(); \
    if (tests_failed == failed_before) { \
        printf("  [PASS]\n"); \
    } \
} while (0)

/* Public callback API unit tests (cache_callbacks.h) */
void test_cstring_key_callbacks(void);
void test_integer_key_callbacks(void);
void test_hash_byte_string_callback(void);
void test_value_purgeable_and_release_callbacks(void);

/* Public core cache API unit tests (cache.h) */
void test_cache_create_and_destroy(void);
void test_cache_set_get_and_release_value(void);
void test_cache_remove_and_remove_all(void);
void test_cache_custom_callbacks_v1(void);
void test_cache_custom_callbacks_v2(void);
void test_cache_scaling_and_integer_keys(void);

#ifdef CACHE_PRIVATE
/* Private cache API unit tests (cache_private.h) */
void test_cache_private_name_and_hints(void);
void test_cache_private_info_and_diagnostics(void);
void test_cache_private_get_and_invoke(void);
void test_cache_private_remove_with_block(void);
void test_cache_private_remove_with_block_callback_false(void);
void test_private_simulate_memory_warning(void);
void test_cache_private_release(void);
void test_cache_private_release_multithreaded(void);
void test_cache_segmented_lru(void);
#endif

#endif /* !_TEST_SUITE_H_ */
