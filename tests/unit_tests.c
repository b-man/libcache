/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>

#include "test_suite.h"

int tests_run = 0;
int tests_failed = 0;

int
main(void)
{
    printf("========================================================================\n");
#ifdef CACHE_PRIVATE
    printf("        LIBCACHE PUBLIC & PRIVATE API UNIT TEST SUITE                   \n");
#else
    printf("             LIBCACHE PUBLIC API UNIT TEST SUITE                        \n");
#endif
    printf("========================================================================\n\n");

    /* Public callback API tests (cache_callbacks.h) */
    RUN_TEST(test_cstring_key_callbacks);
    RUN_TEST(test_integer_key_callbacks);
    RUN_TEST(test_hash_byte_string_callback);
    RUN_TEST(test_value_purgeable_and_release_callbacks);

    /* Public core cache API tests (cache.h) */
    RUN_TEST(test_cache_create_and_destroy);
    RUN_TEST(test_cache_set_get_and_release_value);
    RUN_TEST(test_cache_remove_and_remove_all);
    RUN_TEST(test_cache_custom_callbacks_v1);
    RUN_TEST(test_cache_custom_callbacks_v2);
    RUN_TEST(test_cache_scaling_and_integer_keys);

#ifdef CACHE_PRIVATE
    /* Private cache API tests (cache_private.h) */
    RUN_TEST(test_cache_private_name_and_hints);
    RUN_TEST(test_cache_private_info_and_diagnostics);
    RUN_TEST(test_cache_private_get_and_invoke);
    RUN_TEST(test_cache_private_remove_with_block);
    RUN_TEST(test_cache_private_remove_with_block_callback_false);
    RUN_TEST(test_private_simulate_memory_warning);
    RUN_TEST(test_cache_private_release);
    RUN_TEST(test_cache_private_release_multithreaded);
#endif

    printf("\n========================================================================\n");
    printf("  TEST SUMMARY: Total Run: %d | Total Failed: %d\n",
        tests_run, tests_failed);
    printf("========================================================================\n");

    return (tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
