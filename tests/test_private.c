/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#include <cache.h>
#include <cache_private.h>
#include <cache_callbacks.h>

#include "test_suite.h"

typedef struct {
    int invoke_calls;
    bool last_exists;
    void *last_key;
    void *last_value;
    const char *materialize_str;
    cache_t *active_cache;
    int retain_count_during_hit;
} invoke_test_ctx_t;

static void
test_invoke_callback(void *key, void **value_ptr, bool exists, void *context)
{
    invoke_test_ctx_t *ctx = (invoke_test_ctx_t *)context;
    if (ctx == NULL)
        return;

    ctx->invoke_calls++;
    ctx->last_exists = exists;
    ctx->last_key = key;

    if (exists) {
        ctx->last_value = (value_ptr != NULL) ? *value_ptr : NULL;
        if (ctx->active_cache != NULL) {
            cache_entry_info_t entry_info;
            if (cache_get_info_for_key(ctx->active_cache, key, &entry_info) == 0) {
                ctx->retain_count_during_hit = entry_info.retain_count;
            }
        }
    } else if (value_ptr != NULL && ctx->materialize_str != NULL) {
        *value_ptr = strdup(ctx->materialize_str);
        ctx->last_value = *value_ptr;
    }
}

/*
 * Validates cache_get_name, cache_set_name, cache_get_minimum_values_hint,
 * cache_set_minimum_values_hint, cache_get_count_hint, cache_set_count_hint,
 * cache_get_cost_hint, and cache_set_cost_hint defined in cache_private.h.
 */
void
test_cache_private_name_and_hints(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    size_t count_hint = 0;
    size_t min_hint = 0;
    cache_cost_t cost_hint = 0;
    cache_info_t info;
    int status;

    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    attrs.key_hash_cb = cache_key_hash_cb_cstring;
    attrs.key_is_equal_cb = cache_key_is_equal_cb_cstring;
    attrs.value_release_cb = cache_release_cb_free;

    status = cache_create("com.darwin.test.private.initial", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create must succeed");

    /* Validate cache_get_name and cache_set_name */
    ASSERT_TRUE(cache_get_name(NULL) == NULL,
        "cache_get_name(NULL) must return NULL");
    ASSERT_TRUE(strcmp(cache_get_name(cache), "com.darwin.test.private.initial") == 0,
        "cache_get_name must return initial cache name");

    ASSERT_TRUE(cache_set_name(NULL, "new_name") == EINVAL,
        "cache_set_name with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_set_name(cache, NULL) == EINVAL,
        "cache_set_name with NULL name must return EINVAL");
    ASSERT_TRUE(cache_set_name(cache, "com.darwin.test.private.renamed") == 0,
        "cache_set_name with valid arguments must return 0");
    ASSERT_TRUE(strcmp(cache_get_name(cache), "com.darwin.test.private.renamed") == 0,
        "cache_get_name must reflect updated name after cache_set_name");

    /* Validate cache_set_minimum_values_hint and cache_get_minimum_values_hint */
    ASSERT_TRUE(cache_set_minimum_values_hint(NULL, 10) == EINVAL,
        "cache_set_minimum_values_hint with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_get_minimum_values_hint(NULL, &min_hint) == EINVAL,
        "cache_get_minimum_values_hint with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_get_minimum_values_hint(cache, NULL) == EINVAL,
        "cache_get_minimum_values_hint with NULL hint_out must return EINVAL");

    ASSERT_TRUE(cache_get_minimum_values_hint(cache, &min_hint) == 0 && min_hint == 0,
        "Default minimum_values_hint must be 0");
    ASSERT_TRUE(cache_set_minimum_values_hint(cache, 42) == 0,
        "cache_set_minimum_values_hint must return 0");
    ASSERT_TRUE(cache_get_minimum_values_hint(cache, &min_hint) == 0 && min_hint == 42,
        "cache_get_minimum_values_hint must return the configured value");

    /* Validate cache_set_count_hint and cache_get_count_hint */
    ASSERT_TRUE(cache_set_count_hint(NULL, 5) == EINVAL,
        "cache_set_count_hint with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_get_count_hint(NULL, &count_hint) == EINVAL,
        "cache_get_count_hint with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_get_count_hint(cache, NULL) == EINVAL,
        "cache_get_count_hint with NULL count_out must return EINVAL");

    ASSERT_TRUE(cache_get_count_hint(cache, &count_hint) == 0 && count_hint == 0,
        "Default count_hint must be 0");
    ASSERT_TRUE(cache_set_count_hint(cache, 2) == 0,
        "cache_set_count_hint(cache, 2) must return 0");
    ASSERT_TRUE(cache_get_count_hint(cache, &count_hint) == 0 && count_hint == 2,
        "cache_get_count_hint must return 2 after configuration");

    /* Validate cache_get_cost_hint and cache_set_cost_hint */
    ASSERT_TRUE(cache_get_cost_hint(NULL, "k1", &cost_hint) == EINVAL,
        "cache_get_cost_hint with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_get_cost_hint(cache, NULL, &cost_hint) == EINVAL,
        "cache_get_cost_hint with NULL key must return EINVAL");
    ASSERT_TRUE(cache_get_cost_hint(cache, "k1", NULL) == EINVAL,
        "cache_get_cost_hint with NULL cost_out must return EINVAL");
    ASSERT_TRUE(cache_get_cost_hint(cache, "missing", &cost_hint) == ENOENT,
        "cache_get_cost_hint for a missing key must return ENOENT");

    ASSERT_TRUE(cache_set_cost_hint(NULL, "k1", 100) == EINVAL,
        "cache_set_cost_hint with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_set_cost_hint(cache, NULL, 100) == EINVAL,
        "cache_set_cost_hint with NULL key must return EINVAL");
    ASSERT_TRUE(cache_set_cost_hint(cache, "missing", 100) == ENOENT,
        "cache_set_cost_hint for a missing key must return ENOENT");

    char *v1 = strdup("val_1");
    char *v2 = strdup("val_2");
    char *v3 = strdup("val_3");
    ASSERT_TRUE(cache_set_and_retain(cache, "k1", v1, 100) == 0,
        "Inserting k1 must succeed");
    ASSERT_TRUE(cache_release_value(cache, v1) == 0,
        "Releasing k1 must succeed");

    ASSERT_TRUE(cache_get_cost_hint(cache, "k1", &cost_hint) == 0 && cost_hint == 100,
        "cache_get_cost_hint must return initial entry cost of 100");

    /* Increase cost via cache_set_cost_hint and verify aggregate cost delta */
    ASSERT_TRUE(cache_set_cost_hint(cache, "k1", 250) == 0,
        "cache_set_cost_hint increasing cost must return 0");
    ASSERT_TRUE(cache_get_cost_hint(cache, "k1", &cost_hint) == 0 && cost_hint == 250,
        "cache_get_cost_hint must return updated cost of 250");
    ASSERT_TRUE(cache_get_info(cache, &info) == 0 && info.current_cost == 250,
        "cache_get_info must reflect increased aggregate cost");

    /* Decrease cost via cache_set_cost_hint and verify aggregate cost delta */
    ASSERT_TRUE(cache_set_cost_hint(cache, "k1", 75) == 0,
        "cache_set_cost_hint decreasing cost must return 0");
    ASSERT_TRUE(cache_get_cost_hint(cache, "k1", &cost_hint) == 0 && cost_hint == 75,
        "cache_get_cost_hint must return decreased cost of 75");
    ASSERT_TRUE(cache_get_info(cache, &info) == 0 && info.current_cost == 75,
        "cache_get_info must reflect decreased aggregate cost");

    /* Insert k2 and k3 with count_hint == 2 to verify LRU tail eviction of k1 */
    ASSERT_TRUE(cache_set_and_retain(cache, "k2", v2, 50) == 0,
        "Inserting k2 must succeed");
    ASSERT_TRUE(cache_release_value(cache, v2) == 0,
        "Releasing k2 must succeed");

    ASSERT_TRUE(cache_set_and_retain(cache, "k3", v3, 25) == 0,
        "Inserting k3 exceeding count_hint of 2 must succeed");
    ASSERT_TRUE(cache_release_value(cache, v3) == 0,
        "Releasing k3 must succeed");

    void *lookup_val = NULL;
    ASSERT_TRUE(cache_get(cache, "k1", &lookup_val) == ENOENT,
        "Oldest unretained entry k1 must be evicted when count_hint is exceeded");
    ASSERT_TRUE(cache_get(cache, "k2", &lookup_val) == 0 && lookup_val == v2,
        "Newer entry k2 must remain in cache");
    ASSERT_TRUE(cache_get(cache, "k3", &lookup_val) == 0 && lookup_val == v3,
        "Newest entry k3 must remain in cache");

    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy must succeed");
}

/*
 * Validates cache_get_info, cache_get_info_for_key, cache_get_info_for_keys,
 * cache_print, and cache_print_stats defined in cache_private.h.
 */
void
test_cache_private_info_and_diagnostics(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    cache_info_t info;
    cache_entry_info_t entry_info;
    cache_entry_info_t batch_info[4];
    int status;

    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    attrs.key_hash_cb = cache_key_hash_cb_cstring;
    attrs.key_is_equal_cb = cache_key_is_equal_cb_cstring;
    attrs.value_release_cb = cache_release_cb_free;

    status = cache_create("com.darwin.test.private.info", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create must succeed");

    /* Negative Path: Invalid arguments and missing key lookups for info functions */
    ASSERT_TRUE(cache_get_info(NULL, &info) == EINVAL,
        "cache_get_info with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_get_info(cache, NULL) == EINVAL,
        "cache_get_info with NULL info_out must return EINVAL");

    ASSERT_TRUE(cache_get_info_for_key(NULL, "k", &entry_info) == EINVAL,
        "cache_get_info_for_key with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_get_info_for_key(cache, NULL, &entry_info) == EINVAL,
        "cache_get_info_for_key with NULL key must return EINVAL");
    ASSERT_TRUE(cache_get_info_for_key(cache, "k", NULL) == EINVAL,
        "cache_get_info_for_key with NULL info_out must return EINVAL");
    ASSERT_TRUE(cache_get_info_for_key(cache, "missing", &entry_info) == ENOENT,
        "cache_get_info_for_key for missing key must return ENOENT");

    void *sample_keys[2] = { "alpha", "beta" };
    ASSERT_TRUE(cache_get_info_for_keys(NULL, sample_keys, batch_info, 2) == EINVAL,
        "cache_get_info_for_keys with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_get_info_for_keys(cache, NULL, batch_info, 2) == EINVAL,
        "cache_get_info_for_keys with NULL keys must return EINVAL");
    ASSERT_TRUE(cache_get_info_for_keys(cache, sample_keys, NULL, 2) == EINVAL,
        "cache_get_info_for_keys with NULL info_out must return EINVAL");
    ASSERT_TRUE(cache_get_info_for_keys(cache, sample_keys, batch_info, 0) == EINVAL,
        "cache_get_info_for_keys with count 0 must return EINVAL");

    /* Populate cache with one retained entry (alpha) and one unretained entry (beta) */
    char *val_alpha = strdup("alpha_payload");
    char *val_beta = strdup("beta_payload");
    ASSERT_TRUE(cache_set_and_retain(cache, "alpha", val_alpha, 120) == 0,
        "Inserting alpha must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "beta", val_beta, 80) == 0,
        "Inserting beta must succeed");
    ASSERT_TRUE(cache_release_value(cache, val_beta) == 0,
        "Releasing beta must drop its retain count to 0");

    /* Validate cache_get_info */
    ASSERT_TRUE(cache_get_info(cache, &info) == 0,
        "cache_get_info on populated cache must return 0");
    ASSERT_TRUE(info.current_count == 2 && info.current_cost == 200,
        "cache_get_info must report current_count=2 and current_cost=200");

    /* Validate cache_get_info_for_key */
    ASSERT_TRUE(cache_get_info_for_key(cache, "alpha", &entry_info) == 0,
        "cache_get_info_for_key for alpha must return 0");
    ASSERT_TRUE(entry_info.cost == 120 && entry_info.retain_count == 1,
        "alpha must have cost=120 and retain_count=1");

    ASSERT_TRUE(cache_get_info_for_key(cache, "beta", &entry_info) == 0,
        "cache_get_info_for_key for beta must return 0");
    ASSERT_TRUE(entry_info.cost == 80 && entry_info.retain_count == 0,
        "beta must have cost=80 and retain_count=0");

    /* Validate cache_get_info_for_keys when all keys exist (returns 0) */
    status = cache_get_info_for_keys(cache, sample_keys, batch_info, 2);
    ASSERT_TRUE(status == 0,
        "cache_get_info_for_keys must return 0 when all requested keys are found");
    ASSERT_TRUE(batch_info[0].cost == 120 && batch_info[0].retain_count == 1,
        "batch_info[0] must match alpha stats");
    ASSERT_TRUE(batch_info[1].cost == 80 && batch_info[1].retain_count == 0,
        "batch_info[1] must match beta stats");

    /* Validate cache_get_info_for_keys with missing and NULL keys (returns partial count) */
    void *mixed_keys[4] = { "alpha", "nonexistent", NULL, "beta" };
    status = cache_get_info_for_keys(cache, mixed_keys, batch_info, 4);
    ASSERT_TRUE(status == 2,
        "cache_get_info_for_keys must return count of found keys (2) on partial match");
    ASSERT_TRUE(batch_info[0].cost == 120 && batch_info[0].retain_count == 1,
        "Found key alpha in mixed batch must populate cost and retain_count");
    ASSERT_TRUE(batch_info[1].retain_count == -1,
        "Missing non-NULL key in batch must set retain_count to -1");
    ASSERT_TRUE(batch_info[2].cost == 0 && batch_info[2].retain_count == 0,
        "NULL key slot in batch must remain zero-initialized");
    ASSERT_TRUE(batch_info[3].cost == 80 && batch_info[3].retain_count == 0,
        "Found key beta in mixed batch must populate cost and retain_count");

    /* Validate diagnostic print helpers handle NULL and valid caches cleanly */
    cache_print(NULL);
    cache_print_stats(NULL);
    cache_print(cache);
    cache_print_stats(cache);

    ASSERT_TRUE(cache_release_value(cache, val_alpha) == 0,
        "Releasing alpha must succeed");
    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy must succeed");
}

/*
 * Validates cache_get and cache_invoke defined in cache_private.h.
 */
void
test_cache_private_get_and_invoke(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    cache_entry_info_t entry_info;
    invoke_test_ctx_t ctx;
    void *value_out = NULL;
    int status;

    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    attrs.key_hash_cb = cache_key_hash_cb_cstring;
    attrs.key_is_equal_cb = cache_key_is_equal_cb_cstring;
    attrs.value_release_cb = cache_release_cb_free;

    status = cache_create("com.darwin.test.private.get_invoke", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create must succeed");

    /* Negative Path: cache_get invalid arguments and cache miss */
    ASSERT_TRUE(cache_get(NULL, "key", &value_out) == EINVAL,
        "cache_get with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_get(cache, NULL, &value_out) == EINVAL,
        "cache_get with NULL key must return EINVAL");
    ASSERT_TRUE(cache_get(cache, "key", NULL) == EINVAL,
        "cache_get with NULL value_out must return EINVAL");

    value_out = (void *)0xDEADBEEF;
    ASSERT_TRUE(cache_get(cache, "missing_key", &value_out) == ENOENT,
        "cache_get for a missing key must return ENOENT");
    ASSERT_TRUE(value_out == NULL,
        "cache_get must reset *value_out to NULL on cache miss");

    /* Positive Path: cache_get retrieves value without incrementing retain_count */
    char *stored_val = strdup("non_retained_lookup_value");
    ASSERT_TRUE(cache_set_and_retain(cache, "peek_key", stored_val, 32) == 0,
        "Inserting peek_key must succeed");
    ASSERT_TRUE(cache_release_value(cache, stored_val) == 0,
        "Releasing initial hold on peek_key must drop retain_count to 0");

    ASSERT_TRUE(cache_get(cache, "peek_key", &value_out) == 0,
        "cache_get for existing key must return 0");
    ASSERT_TRUE(value_out == stored_val,
        "cache_get must return the stored value pointer");
    ASSERT_TRUE(cache_get_info_for_key(cache, "peek_key", &entry_info) == 0 &&
        entry_info.retain_count == 0,
        "cache_get must not increment the entry's retain_count");

    /* Negative Path: cache_invoke invalid arguments */
    ASSERT_TRUE(cache_invoke(NULL, "k", test_invoke_callback, &ctx) == EINVAL,
        "cache_invoke with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_invoke(cache, NULL, test_invoke_callback, &ctx) == EINVAL,
        "cache_invoke with NULL key must return EINVAL");
    ASSERT_TRUE(cache_invoke(cache, "k", NULL, &ctx) == EINVAL,
        "cache_invoke with NULL callback must return EINVAL");

    /* Positive Path: cache_invoke on existing key pins entry during callback */
    memset(&ctx, 0, sizeof(ctx));
    ctx.active_cache = cache;
    status = cache_invoke(cache, "peek_key", test_invoke_callback, &ctx);
    ASSERT_TRUE(status == 0,
        "cache_invoke on existing key must return 0");
    ASSERT_TRUE(ctx.invoke_calls == 1 && ctx.last_exists == true,
        "cache_invoke on existing key must invoke callback with exists=true");
    ASSERT_TRUE(ctx.last_value == stored_val,
        "cache_invoke on hit must pass pointer to existing value");
    ASSERT_TRUE(ctx.retain_count_during_hit == 1,
        "cache_invoke must hold a retain reference during callback execution");
    ASSERT_TRUE(cache_get_info_for_key(cache, "peek_key", &entry_info) == 0 &&
        entry_info.retain_count == 0,
        "cache_invoke must release its temporary retain reference after callback returns");

    /* Positive Path: cache_invoke on missing key materializes and inserts new value */
    memset(&ctx, 0, sizeof(ctx));
    ctx.materialize_str = "materialized_on_miss";
    status = cache_invoke(cache, "lazy_key", test_invoke_callback, &ctx);
    ASSERT_TRUE(status == 0,
        "cache_invoke must return 0 when callback materializes a non-NULL value on miss");
    ASSERT_TRUE(ctx.invoke_calls == 1 && ctx.last_exists == false,
        "cache_invoke on missing key must invoke callback with exists=false");

    value_out = NULL;
    ASSERT_TRUE(cache_get(cache, "lazy_key", &value_out) == 0 &&
        value_out == ctx.last_value,
        "Value materialized during cache_invoke miss must be stored in cache");
    ASSERT_TRUE(cache_get_info_for_key(cache, "lazy_key", &entry_info) == 0 &&
        entry_info.retain_count == 0,
        "Materialized entry inserted by cache_invoke must have retain_count 0 after completion");

    /* Negative Path: cache_invoke on missing key when callback leaves *value_ptr NULL */
    memset(&ctx, 0, sizeof(ctx));
    ctx.materialize_str = NULL;
    status = cache_invoke(cache, "unmaterialized_key", test_invoke_callback, &ctx);
    ASSERT_TRUE(status == ENOENT,
        "cache_invoke must return ENOENT when key is missing and callback leaves value NULL");

    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy must succeed");
}

/*
 * Validates cache_remove_with_block defined in cache_private.h.
 */
void
test_cache_private_remove_with_block(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    cache_info_t info;
    void *value_out = NULL;
    int removed_count;
    int status;

    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    attrs.key_hash_cb = cache_key_hash_cb_cstring;
    attrs.key_is_equal_cb = cache_key_is_equal_cb_cstring;
    attrs.value_release_cb = cache_release_cb_free;

    status = cache_create("com.darwin.test.private.remove_block", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create must succeed");

    /* Negative Path: cache_remove_with_block invalid arguments */
    ASSERT_TRUE(cache_remove_with_block(NULL, ^bool(void *k, void *v) {
        (void)k;
        (void)v;
        return true;
    }) == EINVAL, "cache_remove_with_block with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_remove_with_block(cache, NULL) == EINVAL,
        "cache_remove_with_block with NULL block must return EINVAL");

    /* Populate cache with 3 unretained entries and 1 retained entry */
    char *v_drop1 = strdup("drop_1");
    char *v_drop2 = strdup("drop_2");
    char *v_keep = strdup("keep_unretained");
    char *v_pinned = strdup("drop_retained");

    ASSERT_TRUE(cache_set_and_retain(cache, "drop_a", v_drop1, 10) == 0 &&
        cache_release_value(cache, v_drop1) == 0,
        "Inserting unretained drop_a must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "drop_b", v_drop2, 20) == 0 &&
        cache_release_value(cache, v_drop2) == 0,
        "Inserting unretained drop_b must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "keep_c", v_keep, 30) == 0 &&
        cache_release_value(cache, v_keep) == 0,
        "Inserting unretained keep_c must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "drop_pinned", v_pinned, 40) == 0,
        "Inserting retained drop_pinned must succeed");

    /* Verify cache_remove_with_block removes 0 entries when callback returns false */
    removed_count = cache_remove_with_block(cache, ^bool(void *key, void *value) {
        (void)key;
        (void)value;
        return false;
    });
    ASSERT_TRUE(removed_count == 0,
        "cache_remove_with_block must return 0 when callback returns false");

    /* Remove unretained entries whose key starts with "drop_" */
    removed_count = cache_remove_with_block(cache, ^bool(void *key, void *value) {
        (void)value;
        return (strncmp((const char *)key, "drop_", 5) == 0);
    });
    ASSERT_TRUE(removed_count == 2,
        "cache_remove_with_block must remove exactly the 2 unretained matching entries");

    ASSERT_TRUE(cache_get(cache, "drop_a", &value_out) == ENOENT,
        "drop_a must be removed by cache_remove_with_block");
    ASSERT_TRUE(cache_get(cache, "drop_b", &value_out) == ENOENT,
        "drop_b must be removed by cache_remove_with_block");
    ASSERT_TRUE(cache_get(cache, "keep_c", &value_out) == 0 && value_out == v_keep,
        "Non-matching keep_c must remain in cache");
    ASSERT_TRUE(cache_get(cache, "drop_pinned", &value_out) == 0 && value_out == v_pinned,
        "Retained entry drop_pinned must not be removed by cache_remove_with_block");

    ASSERT_TRUE(cache_get_info(cache, &info) == 0 &&
        info.current_count == 2 && info.current_cost == 70,
        "cache_get_info must report 2 remaining entries with total cost 70");

    ASSERT_TRUE(cache_release_value(cache, v_pinned) == 0,
        "Releasing drop_pinned must succeed");
    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy must succeed");
}

/*
 * Validates cache_remove_with_block when the predicate callback returns false.
 */
void
test_cache_private_remove_with_block_callback_false(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    cache_info_t info;
    void *value_out = NULL;
    int removed_count;
    int status;
    __block int callback_invocations = 0;

    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    attrs.key_hash_cb = cache_key_hash_cb_cstring;
    attrs.key_is_equal_cb = cache_key_is_equal_cb_cstring;
    attrs.value_release_cb = cache_release_cb_free;

    status = cache_create("com.darwin.test.private.remove_block_false", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create must succeed");

    char *v1 = strdup("val_one");
    char *v2 = strdup("val_two");
    char *v3 = strdup("val_three");
    char *v_pinned = strdup("val_pinned");

    ASSERT_TRUE(cache_set_and_retain(cache, "entry_1", v1, 10) == 0 &&
        cache_release_value(cache, v1) == 0,
        "Inserting unretained entry_1 must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "entry_2", v2, 20) == 0 &&
        cache_release_value(cache, v2) == 0,
        "Inserting unretained entry_2 must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "entry_3", v3, 30) == 0 &&
        cache_release_value(cache, v3) == 0,
        "Inserting unretained entry_3 must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "entry_pinned", v_pinned, 40) == 0,
        "Inserting retained entry_pinned must succeed");

    ASSERT_TRUE(cache_get_info(cache, &info) == 0 &&
        info.current_count == 4 && info.current_cost == 100,
        "Cache must contain 4 entries with total cost 100 before remove");

    /* When block returns false for all entries */
    callback_invocations = 0;
    removed_count = cache_remove_with_block(cache, ^bool(void *key, void *value) {
        (void)key;
        (void)value;
        callback_invocations++;
        return false;
    });

    ASSERT_TRUE(removed_count == 0,
        "cache_remove_with_block must return 0 when block always returns false");
    ASSERT_TRUE(callback_invocations == 3,
        "Block must be evaluated for all 3 unretained entries");

    /* Verify all entries remain intact in the cache */
    ASSERT_TRUE(cache_get(cache, "entry_1", &value_out) == 0 && value_out == v1,
        "entry_1 must remain in cache when callback returns false");
    ASSERT_TRUE(cache_get(cache, "entry_2", &value_out) == 0 && value_out == v2,
        "entry_2 must remain in cache when callback returns false");
    ASSERT_TRUE(cache_get(cache, "entry_3", &value_out) == 0 && value_out == v3,
        "entry_3 must remain in cache when callback returns false");
    ASSERT_TRUE(cache_get(cache, "entry_pinned", &value_out) == 0 && value_out == v_pinned,
        "entry_pinned must remain in cache when callback returns false");

    ASSERT_TRUE(cache_get_info(cache, &info) == 0 &&
        info.current_count == 4 && info.current_cost == 100,
        "Cache entry count and cost must remain unchanged after false predicate");

    /* Verify selective return where false preserves specific entries */
    removed_count = cache_remove_with_block(cache, ^bool(void *key, void *value) {
        (void)value;
        return (strcmp((const char *)key, "entry_1") == 0);
    });

    ASSERT_TRUE(removed_count == 1,
        "cache_remove_with_block must remove only entry where predicate was true");
    ASSERT_TRUE(cache_get(cache, "entry_1", &value_out) == ENOENT,
        "entry_1 must be removed");
    ASSERT_TRUE(cache_get(cache, "entry_2", &value_out) == 0 && value_out == v2,
        "entry_2 must remain since callback returned false");
    ASSERT_TRUE(cache_get(cache, "entry_3", &value_out) == 0 && value_out == v3,
        "entry_3 must remain since callback returned false");
    ASSERT_TRUE(cache_get(cache, "entry_pinned", &value_out) == 0 && value_out == v_pinned,
        "entry_pinned must remain intact");

    ASSERT_TRUE(cache_release_value(cache, v_pinned) == 0,
        "Releasing entry_pinned must succeed");
    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy must succeed");
}

/*
 * Validates cache_simulate_memory_warning_event defined in cache_private.h.
 */
void
test_private_simulate_memory_warning(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    cache_info_t info;
    void *value_out = NULL;
    int status;
    int i;

    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    attrs.key_hash_cb = cache_key_hash_cb_cstring;
    attrs.key_is_equal_cb = cache_key_is_equal_cb_cstring;
    attrs.value_release_cb = cache_release_cb_free;

    status = cache_create("com.darwin.test.private.memory_warning", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create must succeed");

    /* Populate cache with 1 unretained entry and 1 retained entry */
    char *v_keep = strdup("keep_unretained");
    char *v_pinned = strdup("drop_retained");

    ASSERT_TRUE(cache_set_and_retain(cache, "keep_c", v_keep, 30) == 0 &&
        cache_release_value(cache, v_keep) == 0,
        "Inserting unretained keep_c must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "drop_pinned", v_pinned, 40) == 0,
        "Inserting retained drop_pinned must succeed");

    ASSERT_TRUE(cache_get_info(cache, &info) == 0 &&
        info.current_count == 2 && info.current_cost == 70,
        "cache_get_info must report 2 entries with total cost 70 before memory warning");

    /* Validate cache_simulate_memory_warning_event error handling and execution */
    ASSERT_TRUE(cache_simulate_memory_warning_event(0) == EINVAL,
        "cache_simulate_memory_warning_event(0) must return EINVAL");
    ASSERT_TRUE(cache_simulate_memory_warning_event(0x8000UL) == EINVAL,
        "cache_simulate_memory_warning_event with invalid flag bits must return EINVAL");

    /* Verify WARN event purges unretained keep_c and preserves retained drop_pinned */
    status = cache_simulate_memory_warning_event(DISPATCH_MEMORYPRESSURE_WARN);
    ASSERT_TRUE(status == 0,
        "cache_simulate_memory_warning_event with WARN flag must return 0");

    for (i = 0; i < 100; i++) {
        if (cache_get_info(cache, &info) == 0 && info.current_count == 1)
            break;
        usleep(1000);
    }

    ASSERT_TRUE(cache_get(cache, "keep_c", &value_out) == ENOENT,
        "WARN memory warning event must purge unretained keep_c entry");
    ASSERT_TRUE(cache_get(cache, "drop_pinned", &value_out) == 0 &&
        value_out == v_pinned,
        "WARN memory warning event must preserve retained drop_pinned entry");
    ASSERT_TRUE(cache_get_info(cache, &info) == 0 &&
        info.current_count == 1 && info.current_cost == 40,
        "cache_get_info after WARN event must report 1 remaining entry with cost 40");

    /* Insert a new unretained entry and verify CRITICAL event purges it */
    char *v_crit = strdup("crit_unretained");
    ASSERT_TRUE(v_crit != NULL,
        "Allocating v_crit test value must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "crit_c", v_crit, 25) == 0 &&
        cache_release_value(cache, v_crit) == 0,
        "Inserting unretained crit_c before CRITICAL event must succeed");
    ASSERT_TRUE(cache_get_info(cache, &info) == 0 &&
        info.current_count == 2 && info.current_cost == 65,
        "cache_get_info before CRITICAL event must report 2 entries with cost 65");

    status = cache_simulate_memory_warning_event(DISPATCH_MEMORYPRESSURE_CRITICAL);
    ASSERT_TRUE(status == 0,
        "cache_simulate_memory_warning_event with CRITICAL flag must return 0");

    for (i = 0; i < 100; i++) {
        if (cache_get_info(cache, &info) == 0 && info.current_count == 1)
            break;
        usleep(1000);
    }

    ASSERT_TRUE(cache_get(cache, "crit_c", &value_out) == ENOENT,
        "CRITICAL memory warning event must purge unretained crit_c entry");
    ASSERT_TRUE(cache_get(cache, "drop_pinned", &value_out) == 0 &&
        value_out == v_pinned,
        "CRITICAL memory warning event must preserve retained drop_pinned entry");
    ASSERT_TRUE(cache_get_info(cache, &info) == 0 &&
        info.current_count == 1 && info.current_cost == 40,
        "cache_get_info after CRITICAL event must report 1 remaining entry with cost 40");

    ASSERT_TRUE(cache_release_value(cache, v_pinned) == 0,
        "Releasing drop_pinned must succeed");
    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy must succeed");
}

typedef struct {
    int keys_released;
    int values_released;
} release_test_tracker_t;

static void
release_test_key_retain_cb(void *key_in, void **key_out, void *user_data)
{
    (void)user_data;
    if (key_out != NULL)
        *key_out = (key_in != NULL) ? strdup((const char *)key_in) : NULL;
}

static void
release_test_key_release_cb(void *key, void *user_data)
{
    release_test_tracker_t *tracker = (release_test_tracker_t *)user_data;
    if (tracker != NULL)
        tracker->keys_released++;
    free(key);
}

static void
release_test_value_release_cb(void *value, void *user_data)
{
    release_test_tracker_t *tracker = (release_test_tracker_t *)user_data;
    if (tracker != NULL)
        tracker->values_released++;
    free(value);
}

/*
 * Validates cache_release defined in cache_private.h.
 */
void
test_cache_private_release(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    release_test_tracker_t tracker;
    void *value_out = NULL;
    int status;

    /* Negative Path: NULL cache handle */
    cache_retain(NULL);
    cache_release(NULL);

    memset(&tracker, 0, sizeof(tracker));
    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    attrs.key_hash_cb = cache_key_hash_cb_cstring;
    attrs.key_is_equal_cb = cache_key_is_equal_cb_cstring;
    attrs.key_retain_cb = release_test_key_retain_cb;
    attrs.key_release_cb = release_test_key_release_cb;
    attrs.value_release_cb = release_test_value_release_cb;
    attrs.user_data = &tracker;

    /*
     * Positive Path 1: Explicit cache_retain increments cache reference count,
     * requiring a matching cache_release before the cache is deallocated.
     */
    status = cache_create("com.darwin.test.private.release.immediate", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create for immediate release test must succeed");

    char *val_immediate = strdup("immediate_unretained_val");
    ASSERT_TRUE(val_immediate != NULL,
        "Allocating val_immediate must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "k_imm", val_immediate, 10) == 0 &&
        cache_release_value(cache, val_immediate) == 0,
        "Inserting and releasing k_imm to retain_count 0 must succeed");
    ASSERT_TRUE(tracker.keys_released == 0 && tracker.values_released == 0,
        "Unretained entry must remain cached before cache_release");

    cache_retain(cache);
    cache_release(cache);
    ASSERT_TRUE(tracker.keys_released == 0 && tracker.values_released == 0,
        "Cache must remain alive while extra cache_retain reference is held");

    cache_release(cache);
    ASSERT_TRUE(tracker.keys_released == 1 && tracker.values_released == 1,
        "cache_release must deallocate cache immediately when reference count drops to 0");

    /*
     * Positive Path 2: When entries have retain_count > 0, cache_release
     * defers deallocating the cache instance until all entries reach a
     * reference count of zero.
     */
    memset(&tracker, 0, sizeof(tracker));
    cache = NULL;
    status = cache_create("com.darwin.test.private.release.deferred", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create for deferred release test must succeed");

    char *val_unretained = strdup("active_unretained_val");
    char *val_retained = strdup("active_retained_val");
    char *val_removed = strdup("removed_retained_val");
    ASSERT_TRUE(val_unretained != NULL && val_retained != NULL && val_removed != NULL,
        "Allocating test payloads must succeed");

    /* k_unretained has retain_count == 0 */
    ASSERT_TRUE(cache_set_and_retain(cache, "k_unretained", val_unretained, 10) == 0 &&
        cache_release_value(cache, val_unretained) == 0,
        "Inserting and releasing k_unretained must succeed");

    /* k_retained has retain_count == 2 */
    ASSERT_TRUE(cache_set_and_retain(cache, "k_retained", val_retained, 20) == 0 &&
        cache_get_and_retain(cache, "k_retained", &value_out) == 0 &&
        value_out == val_retained,
        "Inserting and retaining k_retained twice must succeed");

    /* k_removed has retain_count == 1 and is unlinked via cache_remove */
    ASSERT_TRUE(cache_set_and_retain(cache, "k_removed", val_removed, 30) == 0 &&
        cache_remove(cache, "k_removed") == 0,
        "Inserting and removing retained k_removed must succeed");

    /* Call cache_release while entries still have non-zero retain counts */
    cache_release(cache);
    ASSERT_TRUE(tracker.keys_released == 0 && tracker.values_released == 0,
        "cache_release must defer deallocating the cache while entries remain retained");

    /* Decrement k_retained from 2 to 1: cache must still remain alive */
    ASSERT_TRUE(cache_release_value(cache, val_retained) == 0,
        "First cache_release_value on k_retained must return 0");
    ASSERT_TRUE(tracker.keys_released == 0 && tracker.values_released == 0,
        "Cache deallocation must remain deferred while k_retained and k_removed are retained");

    /* Decrement k_removed from 1 to 0: k_removed finalizes, cache still waits on k_retained */
    ASSERT_TRUE(cache_release_value(cache, val_removed) == 0,
        "cache_release_value on k_removed must return 0");
    ASSERT_TRUE(tracker.keys_released == 1 && tracker.values_released == 1,
        "Only k_removed must be finalized while k_retained still has retain_count 1");

    /* Decrement k_retained from 1 to 0: all entries now at 0, triggering cache deallocation */
    ASSERT_TRUE(cache_release_value(cache, val_retained) == 0,
        "Final cache_release_value on k_retained must return 0");
    ASSERT_TRUE(tracker.keys_released == 3 && tracker.values_released == 3,
        "Cache must be deallocated once all entries reach a reference count of zero");

    /*
     * Positive Path 3: Direct comparison with cache_destroy (forced dissolution).
     * Unlike cache_release, cache_destroy forcefully invalidates remaining
     * retain balances and calls cache_release to tear down the cache immediately.
     */
    memset(&tracker, 0, sizeof(tracker));
    cache = NULL;
    status = cache_create("com.darwin.test.private.release.vs_destroy", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create for forced dissolution comparison test must succeed");

    char *val_forced_live = strdup("forced_live_retained");
    char *val_forced_removed = strdup("forced_removed_retained");
    ASSERT_TRUE(val_forced_live != NULL && val_forced_removed != NULL,
        "Allocating forced dissolution test payloads must succeed");

    ASSERT_TRUE(cache_set_and_retain(cache, "k_forced_live", val_forced_live, 15) == 0 &&
        cache_get_and_retain(cache, "k_forced_live", &value_out) == 0,
        "Inserting and retaining k_forced_live must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "k_forced_removed", val_forced_removed, 25) == 0 &&
        cache_remove(cache, "k_forced_removed") == 0,
        "Inserting and removing retained k_forced_removed must succeed");

    status = cache_destroy(cache);
    ASSERT_TRUE(status == 0,
        "cache_destroy with active retained entries must return 0");
    ASSERT_TRUE(tracker.keys_released == 2 && tracker.values_released == 2,
        "cache_destroy must immediately evict and release all entries via cache_release");
}

#define MT_RELEASE_NUM_THREADS     8
#define MT_RELEASE_SENTINEL_COUNT  4
#define MT_RACE_ENTRIES_PER_THREAD 16

typedef struct {
    atomic_int keys_released;
    atomic_int values_released;
    atomic_int sentinels_released;
    atomic_int live_released;
    atomic_int removed_released;
    atomic_int active_retains_remaining;
    atomic_int owner_released;
    atomic_int premature_teardown_errors;
} mt_release_tracker_t;

typedef struct {
    cache_t              *cache;
    mt_release_tracker_t *tracker;
    pthread_mutex_t       mutex;
    pthread_cond_t        phase_cond;
    pthread_cond_t        coord_cond;
    int                   phase1_ready;
    int                   phase2_go;
    int                   last_thread_ready;
    int                   phase3_go;
    atomic_int            worker_errors;
} mt_staggered_ctx_t;

typedef struct {
    mt_staggered_ctx_t *ctx;
    int                 thread_id;
} mt_staggered_arg_t;

typedef struct {
    cache_t              *cache;
    mt_release_tracker_t *tracker;
    pthread_mutex_t       mutex;
    pthread_cond_t        ready_cond;
    pthread_cond_t        start_cond;
    int                   ready_count;
    int                   start_flag;
    void                 *retained_vals[MT_RELEASE_NUM_THREADS][MT_RACE_ENTRIES_PER_THREAD];
    atomic_int            worker_errors;
} mt_race_ctx_t;

typedef struct {
    mt_race_ctx_t *ctx;
    int            thread_id;
} mt_race_arg_t;

static void
mt_release_key_retain_cb(void *key_in, void **key_out, void *user_data)
{
    (void)user_data;
    if (key_out != NULL)
        *key_out = (key_in != NULL) ? strdup((const char *)key_in) : NULL;
}

static void
mt_release_key_release_cb(void *key, void *user_data)
{
    mt_release_tracker_t *tracker = (mt_release_tracker_t *)user_data;
    const char *key_str = (const char *)key;

    if (tracker != NULL && key_str != NULL) {
        if (strncmp(key_str, "sentinel_", 9) == 0) {
            if (atomic_load(&tracker->active_retains_remaining) > 0 ||
                atomic_load(&tracker->owner_released) == 0) {
                atomic_fetch_add(&tracker->premature_teardown_errors, 1);
            }
            atomic_fetch_add(&tracker->sentinels_released, 1);
            atomic_fetch_add(&tracker->live_released, 1);
        } else if (strncmp(key_str, "worker_active_", 14) == 0 ||
                   strncmp(key_str, "race_entry_", 11) == 0) {
            if (atomic_load(&tracker->active_retains_remaining) > 0 ||
                atomic_load(&tracker->owner_released) == 0) {
                atomic_fetch_add(&tracker->premature_teardown_errors, 1);
            }
            atomic_fetch_add(&tracker->live_released, 1);
        } else if (strncmp(key_str, "worker_removed_", 15) == 0) {
            atomic_fetch_add(&tracker->removed_released, 1);
        }
        atomic_fetch_add(&tracker->keys_released, 1);
    }
    free(key);
}

static void
mt_release_value_release_cb(void *value, void *user_data)
{
    mt_release_tracker_t *tracker = (mt_release_tracker_t *)user_data;
    if (tracker != NULL)
        atomic_fetch_add(&tracker->values_released, 1);
    free(value);
}

static void *
mt_staggered_worker(void *arg)
{
    mt_staggered_arg_t *warg = (mt_staggered_arg_t *)arg;
    mt_staggered_ctx_t *ctx = warg->ctx;
    int tid = warg->thread_id;
    char key_active[64];
    char key_removed[64];
    char *val_active = NULL;
    char *val_removed = NULL;
    void *val_dup = NULL;

    snprintf(key_active, sizeof(key_active), "worker_active_%d", tid);
    snprintf(key_removed, sizeof(key_removed), "worker_removed_%d", tid);

    val_active = strdup(key_active);
    val_removed = strdup(key_removed);
    if (val_active == NULL || val_removed == NULL) {
        free(val_active);
        free(val_removed);
        atomic_fetch_add(&ctx->worker_errors, 1);
        return (NULL);
    }

    /* Insert and retain active entry twice (retain_count == 2) */
    if (cache_set_and_retain(ctx->cache, key_active, val_active, 10) != 0 ||
        cache_get_and_retain(ctx->cache, key_active, &val_dup) != 0 ||
        val_dup != val_active) {
        atomic_fetch_add(&ctx->worker_errors, 1);
    }

    /* Insert, retain, and unlink removed entry (retain_count == 1 on removed_head) */
    if (cache_set_and_retain(ctx->cache, key_removed, val_removed, 15) != 0 ||
        cache_remove(ctx->cache, key_removed) != 0) {
        atomic_fetch_add(&ctx->worker_errors, 1);
    }

    /* Signal Phase 1 ready and wait for coordinator to call cache_release(cache) */
    pthread_mutex_lock(&ctx->mutex);
    ctx->phase1_ready++;
    if (ctx->phase1_ready == MT_RELEASE_NUM_THREADS)
        pthread_cond_signal(&ctx->coord_cond);
    while (!ctx->phase2_go)
        pthread_cond_wait(&ctx->phase_cond, &ctx->mutex);
    pthread_mutex_unlock(&ctx->mutex);

    /* Phase 2: Release secondary active hold and removed entry hold concurrently */
    if (cache_release_value(ctx->cache, val_dup) != 0)
        atomic_fetch_add(&ctx->worker_errors, 1);
    if (cache_release_value(ctx->cache, val_removed) != 0)
        atomic_fetch_add(&ctx->worker_errors, 1);

    if (tid < MT_RELEASE_NUM_THREADS - 1) {
        /* Threads 0..6 release their final active reference and exit */
        atomic_fetch_sub(&ctx->tracker->active_retains_remaining, 1);
        if (cache_release_value(ctx->cache, val_active) != 0)
            atomic_fetch_add(&ctx->worker_errors, 1);
        return (NULL);
    }

    /* Thread 7 holds the last remaining reference until Phase 3 */
    pthread_mutex_lock(&ctx->mutex);
    ctx->last_thread_ready = 1;
    pthread_cond_signal(&ctx->coord_cond);
    while (!ctx->phase3_go)
        pthread_cond_wait(&ctx->phase_cond, &ctx->mutex);
    pthread_mutex_unlock(&ctx->mutex);

    /* Phase 3: Thread 7 releases the final reference, triggering cache teardown */
    atomic_fetch_sub(&ctx->tracker->active_retains_remaining, 1);
    if (cache_release_value(ctx->cache, val_active) != 0)
        atomic_fetch_add(&ctx->worker_errors, 1);

    return (NULL);
}

static void *
mt_race_worker(void *arg)
{
    mt_race_arg_t *warg = (mt_race_arg_t *)arg;
    mt_race_ctx_t *ctx = warg->ctx;
    int tid = warg->thread_id;
    int i;

    pthread_mutex_lock(&ctx->mutex);
    ctx->ready_count++;
    if (ctx->ready_count == MT_RELEASE_NUM_THREADS)
        pthread_cond_signal(&ctx->ready_cond);
    while (!ctx->start_flag)
        pthread_cond_wait(&ctx->start_cond, &ctx->mutex);
    pthread_mutex_unlock(&ctx->mutex);

    for (i = 0; i < MT_RACE_ENTRIES_PER_THREAD; i++) {
        atomic_fetch_sub(&ctx->tracker->active_retains_remaining, 1);
        if (cache_release_value(ctx->cache, ctx->retained_vals[tid][i]) != 0)
            atomic_fetch_add(&ctx->worker_errors, 1);
    }

    return (NULL);
}

/*
 * Validates multi-threaded behavior of cache_release and verifies that
 * cache container deallocation is deferred across threads until all
 * retained entries reach a reference count of zero.
 */
void
test_cache_private_release_multithreaded(void)
{
    cache_attributes_t attrs;
    mt_release_tracker_t tracker;
    mt_staggered_ctx_t staggered_ctx;
    mt_staggered_arg_t staggered_args[MT_RELEASE_NUM_THREADS];
    pthread_t threads[MT_RELEASE_NUM_THREADS];
    cache_info_t info;
    int i, j, status;

    memset(&tracker, 0, sizeof(tracker));
    atomic_store(&tracker.active_retains_remaining, MT_RELEASE_NUM_THREADS);

    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    attrs.key_hash_cb = cache_key_hash_cb_cstring;
    attrs.key_is_equal_cb = cache_key_is_equal_cb_cstring;
    attrs.key_retain_cb = mt_release_key_retain_cb;
    attrs.key_release_cb = mt_release_key_release_cb;
    attrs.value_release_cb = mt_release_value_release_cb;
    attrs.user_data = &tracker;

    /*
     * Part 1: Phased multi-threaded test verifying that cache_release defers
     * container deallocation while worker threads hold retained entries, and
     * that the final worker thread's cache_release_value deallocates the cache.
     */
    memset(&staggered_ctx, 0, sizeof(staggered_ctx));
    staggered_ctx.tracker = &tracker;
    pthread_mutex_init(&staggered_ctx.mutex, NULL);
    pthread_cond_init(&staggered_ctx.phase_cond, NULL);
    pthread_cond_init(&staggered_ctx.coord_cond, NULL);

    status = cache_create("com.darwin.test.private.release.mt_staggered",
        &attrs, &staggered_ctx.cache);
    ASSERT_TRUE(status == 0 && staggered_ctx.cache != NULL,
        "cache_create for multi-threaded staggered release test must succeed");

    /* Insert unretained sentinel entries that only get freed on cache container teardown */
    for (i = 0; i < MT_RELEASE_SENTINEL_COUNT; i++) {
        char s_key[32];
        char *s_val;
        snprintf(s_key, sizeof(s_key), "sentinel_%d", i);
        s_val = strdup(s_key);
        ASSERT_TRUE(s_val != NULL, "Allocating sentinel value must succeed");
        ASSERT_TRUE(cache_set_and_retain(staggered_ctx.cache, s_key, s_val, 5) == 0 &&
            cache_release_value(staggered_ctx.cache, s_val) == 0,
            "Inserting unretained sentinel entry must succeed");
    }

    /* Spawn worker threads to concurrently insert and retain entries */
    for (i = 0; i < MT_RELEASE_NUM_THREADS; i++) {
        staggered_args[i].ctx = &staggered_ctx;
        staggered_args[i].thread_id = i;
        status = pthread_create(&threads[i], NULL, mt_staggered_worker,
            &staggered_args[i]);
        ASSERT_TRUE(status == 0, "pthread_create for staggered worker must succeed");
    }

    /* Wait until all worker threads hold their retained entries */
    pthread_mutex_lock(&staggered_ctx.mutex);
    while (staggered_ctx.phase1_ready < MT_RELEASE_NUM_THREADS)
        pthread_cond_wait(&staggered_ctx.coord_cond, &staggered_ctx.mutex);
    pthread_mutex_unlock(&staggered_ctx.mutex);

    ASSERT_TRUE(atomic_load(&staggered_ctx.worker_errors) == 0,
        "All worker threads must succeed in retaining entries during Phase 1");

    /* Call cache_release on the main thread while all 8 worker threads hold references */
    atomic_store(&tracker.owner_released, 1);
    cache_release(staggered_ctx.cache);
    ASSERT_TRUE(atomic_load(&tracker.keys_released) == 0 &&
        atomic_load(&tracker.values_released) == 0 &&
        atomic_load(&tracker.sentinels_released) == 0,
        "cache_release must defer cache container deallocation while threads hold entries");

    /* Signal Phase 2: Threads 0..6 release all holds; Thread 7 keeps 1 final hold */
    pthread_mutex_lock(&staggered_ctx.mutex);
    staggered_ctx.phase2_go = 1;
    pthread_cond_broadcast(&staggered_ctx.phase_cond);
    pthread_mutex_unlock(&staggered_ctx.mutex);

    for (i = 0; i < MT_RELEASE_NUM_THREADS - 1; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_lock(&staggered_ctx.mutex);
    while (!staggered_ctx.last_thread_ready)
        pthread_cond_wait(&staggered_ctx.coord_cond, &staggered_ctx.mutex);
    pthread_mutex_unlock(&staggered_ctx.mutex);

    /* Verify that threads 0..6 finished, removed entries were freed, but container is still alive */
    ASSERT_TRUE(atomic_load(&staggered_ctx.worker_errors) == 0,
        "Worker threads must not encounter errors during Phase 2 releases");
    ASSERT_TRUE(atomic_load(&tracker.removed_released) == MT_RELEASE_NUM_THREADS,
        "All unlinked removed entries must be finalized when their retain_count reaches 0");
    ASSERT_TRUE(atomic_load(&tracker.sentinels_released) == 0 &&
        atomic_load(&tracker.live_released) == 0,
        "Cache container deallocation must remain deferred while the last thread holds 1 entry");
    ASSERT_TRUE(cache_get_info(staggered_ctx.cache, &info) == 0 &&
        info.current_count == (size_t)(MT_RELEASE_SENTINEL_COUNT + MT_RELEASE_NUM_THREADS),
        "Deferred cache container must remain intact and queryable while 1 reference remains");

    /* Signal Phase 3: Thread 7 releases the final retained entry, triggering container teardown */
    pthread_mutex_lock(&staggered_ctx.mutex);
    staggered_ctx.phase3_go = 1;
    pthread_cond_broadcast(&staggered_ctx.phase_cond);
    pthread_mutex_unlock(&staggered_ctx.mutex);

    pthread_join(threads[MT_RELEASE_NUM_THREADS - 1], NULL);

    pthread_mutex_destroy(&staggered_ctx.mutex);
    pthread_cond_destroy(&staggered_ctx.phase_cond);
    pthread_cond_destroy(&staggered_ctx.coord_cond);

    ASSERT_TRUE(atomic_load(&staggered_ctx.worker_errors) == 0,
        "Final worker thread must succeed in releasing last retained entry");
    ASSERT_TRUE(atomic_load(&tracker.premature_teardown_errors) == 0,
        "Cache container must not be torn down before all active retains are released");
    ASSERT_TRUE(atomic_load(&tracker.sentinels_released) == MT_RELEASE_SENTINEL_COUNT,
        "All unretained sentinel entries must be evicted when cache container is deallocated");
    ASSERT_TRUE(atomic_load(&tracker.live_released) ==
        MT_RELEASE_SENTINEL_COUNT + MT_RELEASE_NUM_THREADS,
        "All live entries must be evicted and freed upon final deferred cache_release");
    ASSERT_TRUE(atomic_load(&tracker.keys_released) ==
        MT_RELEASE_SENTINEL_COUNT + (MT_RELEASE_NUM_THREADS * 2) &&
        atomic_load(&tracker.values_released) ==
        MT_RELEASE_SENTINEL_COUNT + (MT_RELEASE_NUM_THREADS * 2),
        "All keys and values across all threads must be released once reference count hits 0");

    /*
     * Part 2: High-contention concurrent release race where 8 threads simultaneously
     * release 128 retained entries while the owner thread calls cache_release(cache).
     */
    mt_race_ctx_t race_ctx;
    mt_race_arg_t race_args[MT_RELEASE_NUM_THREADS];

    memset(&tracker, 0, sizeof(tracker));
    atomic_store(&tracker.active_retains_remaining,
        MT_RELEASE_NUM_THREADS * MT_RACE_ENTRIES_PER_THREAD);

    memset(&race_ctx, 0, sizeof(race_ctx));
    race_ctx.tracker = &tracker;
    pthread_mutex_init(&race_ctx.mutex, NULL);
    pthread_cond_init(&race_ctx.ready_cond, NULL);
    pthread_cond_init(&race_ctx.start_cond, NULL);

    status = cache_create("com.darwin.test.private.release.mt_race",
        &attrs, &race_ctx.cache);
    ASSERT_TRUE(status == 0 && race_ctx.cache != NULL,
        "cache_create for multi-threaded release race test must succeed");

    for (i = 0; i < MT_RELEASE_SENTINEL_COUNT; i++) {
        char s_key[32];
        char *s_val;
        snprintf(s_key, sizeof(s_key), "sentinel_%d", i);
        s_val = strdup(s_key);
        ASSERT_TRUE(s_val != NULL, "Allocating race sentinel value must succeed");
        ASSERT_TRUE(cache_set_and_retain(race_ctx.cache, s_key, s_val, 5) == 0 &&
            cache_release_value(race_ctx.cache, s_val) == 0,
            "Inserting race sentinel entry must succeed");
    }

    for (i = 0; i < MT_RELEASE_NUM_THREADS; i++) {
        for (j = 0; j < MT_RACE_ENTRIES_PER_THREAD; j++) {
            char r_key[64];
            char *r_val;
            snprintf(r_key, sizeof(r_key), "race_entry_%d_%d", i, j);
            r_val = strdup(r_key);
            ASSERT_TRUE(r_val != NULL, "Allocating race entry value must succeed");
            ASSERT_TRUE(cache_set_and_retain(race_ctx.cache, r_key, r_val, 10) == 0,
                "Retaining race entry must succeed");
            race_ctx.retained_vals[i][j] = r_val;
        }
    }

    for (i = 0; i < MT_RELEASE_NUM_THREADS; i++) {
        race_args[i].ctx = &race_ctx;
        race_args[i].thread_id = i;
        status = pthread_create(&threads[i], NULL, mt_race_worker, &race_args[i]);
        ASSERT_TRUE(status == 0, "pthread_create for race worker must succeed");
    }

    pthread_mutex_lock(&race_ctx.mutex);
    while (race_ctx.ready_count < MT_RELEASE_NUM_THREADS)
        pthread_cond_wait(&race_ctx.ready_cond, &race_ctx.mutex);
    atomic_store(&tracker.owner_released, 1);
    race_ctx.start_flag = 1;
    pthread_cond_broadcast(&race_ctx.start_cond);
    pthread_mutex_unlock(&race_ctx.mutex);

    /* Concurrently release the cache handle while 8 worker threads release entries */
    cache_release(race_ctx.cache);

    for (i = 0; i < MT_RELEASE_NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&race_ctx.mutex);
    pthread_cond_destroy(&race_ctx.ready_cond);
    pthread_cond_destroy(&race_ctx.start_cond);

    ASSERT_TRUE(atomic_load(&race_ctx.worker_errors) == 0,
        "Concurrent race workers must release all retained values without error");
    ASSERT_TRUE(atomic_load(&tracker.premature_teardown_errors) == 0,
        "Cache container deallocation must never occur while any thread holds a retained entry");
    ASSERT_TRUE(atomic_load(&tracker.sentinels_released) == MT_RELEASE_SENTINEL_COUNT,
        "All sentinel entries must be released when concurrent race teardown completes");
    ASSERT_TRUE(atomic_load(&tracker.keys_released) ==
        MT_RELEASE_SENTINEL_COUNT + (MT_RELEASE_NUM_THREADS * MT_RACE_ENTRIES_PER_THREAD) &&
        atomic_load(&tracker.values_released) ==
        MT_RELEASE_SENTINEL_COUNT + (MT_RELEASE_NUM_THREADS * MT_RACE_ENTRIES_PER_THREAD),
        "All 132 keys and values must be cleanly released after all threads finish");
}

/*
 * Validates Segmented LRU (SLRU) eviction policy:
 * - New entries start in the probationary segment.
 * - Accessed entries are promoted to the protected segment.
 * - Under capacity pressure (count_hint), eviction selects from the probationary tail first,
 *   protecting frequently used entries from being flushed by one-hit wonders.
 * - When the protected segment overflows, its LRU entry is demoted back to the probationary segment.
 */
void
test_cache_segmented_lru(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    void *val_out = NULL;
    int status;

    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    attrs.key_hash_cb = cache_key_hash_cb_cstring;
    attrs.key_is_equal_cb = cache_key_is_equal_cb_cstring;
    attrs.value_release_cb = cache_release_cb_free;

    status = cache_create("com.test.slru", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL, "cache_create for SLRU test must succeed");

    /* Set count hint to 3 */
    status = cache_set_count_hint(cache, 3);
    ASSERT_TRUE(status == 0, "cache_set_count_hint must return 0");

    char *v1 = strdup("val_1");
    char *v2 = strdup("val_2");
    char *v3 = strdup("val_3");
    char *v4 = strdup("val_4");

    /* Insert k1 and release it */
    ASSERT_TRUE(cache_set_and_retain(cache, "k1", v1, 10) == 0, "Inserting k1 must succeed");
    ASSERT_TRUE(cache_release_value(cache, v1) == 0, "Releasing k1 must succeed");

    /* Access k1 via cache_get to promote it to the protected segment */
    ASSERT_TRUE(cache_get(cache, "k1", &val_out) == 0 && val_out == v1, "cache_get for k1 must succeed");

    /* Insert k2 and k3 (one-hit wonders, unreferenced in probationary segment) */
    ASSERT_TRUE(cache_set_and_retain(cache, "k2", v2, 10) == 0, "Inserting k2 must succeed");
    ASSERT_TRUE(cache_release_value(cache, v2) == 0, "Releasing k2 must succeed");

    ASSERT_TRUE(cache_set_and_retain(cache, "k3", v3, 10) == 0, "Inserting k3 must succeed");
    ASSERT_TRUE(cache_release_value(cache, v3) == 0, "Releasing k3 must succeed");

    /*
     * At this point:
     * - k1 was accessed and promoted to protected segment.
     * - k2 was inserted then k3 was inserted into probationary segment.
     * In single-queue LRU: k1 is the oldest accessed entry (accessed before k2 and k3 were inserted).
     * In Segmented LRU: k1 is protected; k2 is the tail of the probationary segment.
     *
     * Insert k4 (count_hint = 3 exceeded).
     * SLRU must evict k2 (probationary tail) instead of k1 (protected)!
     */
    ASSERT_TRUE(cache_set_and_retain(cache, "k4", v4, 10) == 0, "Inserting k4 must succeed");
    ASSERT_TRUE(cache_release_value(cache, v4) == 0, "Releasing k4 must succeed");

    ASSERT_TRUE(cache_get(cache, "k1", &val_out) == 0 && val_out == v1,
        "Frequently accessed entry k1 in protected segment must NOT be evicted");
    ASSERT_TRUE(cache_get(cache, "k2", &val_out) == ENOENT,
        "One-hit entry k2 at probationary tail must be evicted");
    ASSERT_TRUE(cache_get(cache, "k3", &val_out) == 0 && val_out == v3,
        "Newer entry k3 must remain in cache");
    ASSERT_TRUE(cache_get(cache, "k4", &val_out) == 0 && val_out == v4,
        "Newest entry k4 must remain in cache");

    ASSERT_TRUE(cache_destroy(cache) == 0, "cache_destroy must succeed");
}

