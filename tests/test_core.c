/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <cache.h>
#include <cache_callbacks.h>

#include "test_suite.h"

typedef struct {
    int hash_calls;
    int equal_calls;
    int key_retain_calls;
    int key_release_calls;
    int value_retain_calls;
    int value_release_calls;
    int make_purgeable_calls;
    int make_nonpurgeable_calls;
    bool simulate_purged;
} callback_tracker_t;

static uintptr_t
custom_key_hash_cb(void *key, void *user_data)
{
    callback_tracker_t *tracker = (callback_tracker_t *)user_data;
    if (tracker != NULL) {
        tracker->hash_calls++;
    }
    return cache_key_hash_cb_cstring(key, NULL);
}

static bool
custom_key_is_equal_cb(void *key1, void *key2, void *user_data)
{
    callback_tracker_t *tracker = (callback_tracker_t *)user_data;
    if (tracker != NULL) {
        tracker->equal_calls++;
    }
    return cache_key_is_equal_cb_cstring(key1, key2, NULL);
}

static void
custom_key_retain_cb(void *key_in, void **key_out, void *user_data)
{
    callback_tracker_t *tracker = (callback_tracker_t *)user_data;
    if (tracker != NULL) {
        tracker->key_retain_calls++;
    }
    if (key_out != NULL) {
        *key_out = (key_in != NULL) ? strdup((const char *)key_in) : NULL;
    }
}

static void
custom_key_release_cb(void *key, void *user_data)
{
    callback_tracker_t *tracker = (callback_tracker_t *)user_data;
    if (tracker != NULL) {
        tracker->key_release_calls++;
    }
    free(key);
}

static void
custom_value_retain_cb(void *value_in, void *user_data)
{
    (void)value_in;
    callback_tracker_t *tracker = (callback_tracker_t *)user_data;
    if (tracker != NULL) {
        tracker->value_retain_calls++;
    }
}

static void
custom_value_release_cb(void *value, void *user_data)
{
    callback_tracker_t *tracker = (callback_tracker_t *)user_data;
    if (tracker != NULL) {
        tracker->value_release_calls++;
    }
    free(value);
}

static void
custom_value_make_purgeable_cb(void *value, void *user_data)
{
    (void)value;
    callback_tracker_t *tracker = (callback_tracker_t *)user_data;
    if (tracker != NULL) {
        tracker->make_purgeable_calls++;
    }
}

static bool
custom_value_make_nonpurgeable_cb(void *value, void *user_data)
{
    (void)value;
    callback_tracker_t *tracker = (callback_tracker_t *)user_data;
    if (tracker != NULL) {
        tracker->make_nonpurgeable_calls++;
        if (tracker->simulate_purged) {
            return false;
        }
    }
    return true;
}

/*
 * Validates cache_create and cache_destroy defined in cache.h.
 */
void
test_cache_create_and_destroy(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    int status;

    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_1;
    attrs.key_hash_cb = cache_key_hash_cb_cstring;
    attrs.key_is_equal_cb = cache_key_is_equal_cb_cstring;
    attrs.value_release_cb = cache_release_cb_free;

    /* Positive Path: Create and destroy with CACHE_ATTRIBUTES_VERSION_1 */
    status = cache_create("com.darwin.test.create.v1", &attrs, &cache);
    ASSERT_TRUE(status == 0,
        "cache_create with VERSION_1 attributes must return 0");
    ASSERT_TRUE(cache != NULL,
        "cache_create must populate non-NULL cache handle");

    status = cache_destroy(cache);
    ASSERT_TRUE(status == 0,
        "cache_destroy on a valid cache handle must return 0");

    /* Positive Path: Create and destroy with CACHE_ATTRIBUTES_VERSION_2 */
    cache = NULL;
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    status = cache_create("com.darwin.test.create.v2", &attrs, &cache);
    ASSERT_TRUE(status == 0,
        "cache_create with VERSION_2 attributes must return 0");
    ASSERT_TRUE(cache != NULL,
        "cache_create with VERSION_2 must populate non-NULL cache handle");
    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy on VERSION_2 cache must return 0");

    /* Negative Path: NULL name pointer */
    cache = NULL;
    status = cache_create(NULL, &attrs, &cache);
    ASSERT_TRUE(status == EINVAL,
        "cache_create with NULL name must return EINVAL");

    /* Negative Path: NULL attributes pointer */
    cache = NULL;
    status = cache_create("com.darwin.test.invalid_attrs", NULL, &cache);
    ASSERT_TRUE(status == EINVAL,
        "cache_create with NULL attrs must return EINVAL");

    /* Negative Path: NULL cache_out pointer */
    status = cache_create("com.darwin.test.invalid_out", &attrs, NULL);
    ASSERT_TRUE(status == EINVAL,
        "cache_create with NULL cache_out must return EINVAL");

    /* Positive Path: NULL key_hash_cb and key_is_equal_cb use default pointer/integer callbacks */
    cache = NULL;
    cache_attributes_t default_hash = attrs;
    default_hash.key_hash_cb = NULL;
    status = cache_create("com.darwin.test.default_hash", &default_hash, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create with NULL key_hash_cb must succeed using default integer hash");
    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy on cache with default key_hash_cb must return 0");

    cache = NULL;
    cache_attributes_t default_equal = attrs;
    default_equal.key_is_equal_cb = NULL;
    status = cache_create("com.darwin.test.default_equal", &default_equal, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create with NULL key_is_equal_cb must succeed using default pointer equality");
    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy on cache with default key_is_equal_cb must return 0");

    /* Edge Case: Non-standard attribute version numbers fall through and assume VERSION_1 */
    callback_tracker_t fallback_tracker;
    memset(&fallback_tracker, 0, sizeof(fallback_tracker));
    cache_attributes_t fallback_attrs = attrs;
    fallback_attrs.version = 0;
    fallback_attrs.user_data = &fallback_tracker;
    fallback_attrs.value_retain_cb = custom_value_retain_cb;
    fallback_attrs.value_release_cb = custom_value_release_cb;
    cache = NULL;
    status = cache_create("com.darwin.test.version0", &fallback_attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create with version 0 must succeed by assuming VERSION_1");
    char *v0_val = strdup("v0_payload");
    ASSERT_TRUE(v0_val != NULL, "Payload allocation must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "v0_key", v0_val, 1) == 0,
        "cache_set_and_retain on version 0 cache must succeed");
    ASSERT_TRUE(fallback_tracker.value_retain_calls == 0,
        "version 0 cache must assume VERSION_1 and ignore value_retain_cb");
    ASSERT_TRUE(cache_release_value(cache, v0_val) == 0,
        "cache_release_value on version 0 cache must return 0");
    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy on version 0 cache must return 0");

    cache = NULL;
    fallback_attrs.version = 99;
    status = cache_create("com.darwin.test.version99", &fallback_attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create with unsupported version must succeed by assuming VERSION_1");
    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy on unsupported version cache must return 0");
}

/*
 * Validates cache_set_and_retain, cache_get_and_retain, and cache_release_value
 * defined in cache.h.
 */
void
test_cache_set_get_and_release_value(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    void *value_out = NULL;
    int status;

    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    attrs.key_hash_cb = cache_key_hash_cb_cstring;
    attrs.key_is_equal_cb = cache_key_is_equal_cb_cstring;
    attrs.key_retain_cb = custom_key_retain_cb;
    attrs.key_release_cb = cache_release_cb_free;
    attrs.value_release_cb = cache_release_cb_free;

    status = cache_create("com.darwin.test.set_get_release", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create must succeed");

    char *payload = strdup("cached_value_payload");
    ASSERT_TRUE(payload != NULL, "Payload allocation must succeed");

    /* Positive Path: Insert entry with initial retain count of 1 */
    status = cache_set_and_retain(cache, "primary_key", payload, 64);
    ASSERT_TRUE(status == 0,
        "cache_set_and_retain must return 0 for valid arguments");

    /* Positive Path: Retrieve entry and increment retain count */
    status = cache_get_and_retain(cache, "primary_key", &value_out);
    ASSERT_TRUE(status == 0,
        "cache_get_and_retain must return 0 for an existing key");
    ASSERT_TRUE(value_out == payload,
        "cache_get_and_retain must return the exact stored value pointer");

    /* Positive Path: Release reference from get_and_retain and from set_and_retain */
    status = cache_release_value(cache, value_out);
    ASSERT_TRUE(status == 0,
        "First cache_release_value must return 0");

    status = cache_release_value(cache, payload);
    ASSERT_TRUE(status == 0,
        "Second cache_release_value dropping retain count to 0 must return 0");

    /* Positive Path: Re-setting same key with same value pointer retains and updates cost */
    status = cache_set_and_retain(cache, "primary_key", payload, 96);
    ASSERT_TRUE(status == 0,
        "Re-setting existing key with identical value pointer must return 0");
    ASSERT_TRUE(cache_release_value(cache, payload) == 0,
        "Releasing re-set reference on same value pointer must return 0");

    /* Positive Path: Overwriting an existing key replaces its value */
    char *replacement_payload = strdup("updated_value_payload");
    ASSERT_TRUE(replacement_payload != NULL, "Replacement allocation must succeed");
    status = cache_set_and_retain(cache, "primary_key", replacement_payload, 128);
    ASSERT_TRUE(status == 0,
        "Overwriting an existing key with cache_set_and_retain must return 0");

    value_out = NULL;
    status = cache_get_and_retain(cache, "primary_key", &value_out);
    ASSERT_TRUE(status == 0 && value_out == replacement_payload,
        "Lookup after key overwrite must return the new value");
    ASSERT_TRUE(cache_release_value(cache, value_out) == 0,
        "Releasing lookup reference on replacement value must return 0");
    ASSERT_TRUE(cache_release_value(cache, replacement_payload) == 0,
        "Releasing set reference on replacement value must return 0");

    /* Negative Path: Cache miss returns ENOENT and sets *value_out to NULL */
    void *sentinel = (void *)0xDEADBEEF;
    value_out = sentinel;
    status = cache_get_and_retain(cache, "nonexistent_key", &value_out);
    ASSERT_TRUE(status == ENOENT,
        "cache_get_and_retain for a missing key must return ENOENT");
    ASSERT_TRUE(value_out == NULL,
        "cache_get_and_retain must set *value_out to NULL on cache miss");

    /* Boundary Path: Associating a key with a NULL value succeeds */
    ASSERT_TRUE(cache_set_and_retain(cache, "null_value_key", NULL, 0) == 0,
        "cache_set_and_retain with NULL value must succeed and associate key with NULL");
    value_out = sentinel;
    ASSERT_TRUE(cache_get_and_retain(cache, "null_value_key", &value_out) == 0,
        "cache_get_and_retain for a key associated with NULL must return 0");
    ASSERT_TRUE(value_out == NULL,
        "cache_get_and_retain for a key associated with NULL must set *value_out to NULL");
    ASSERT_TRUE(cache_remove(cache, "null_value_key") == 0,
        "cache_remove for a key associated with NULL must return 0");

    /* Negative Path: Invalid arguments to cache_set_and_retain */
    ASSERT_TRUE(cache_set_and_retain(NULL, "k", replacement_payload, 1) == EINVAL,
        "cache_set_and_retain with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_set_and_retain(cache, NULL, replacement_payload, 1) == EINVAL,
        "cache_set_and_retain with NULL key must return EINVAL");

    /* Negative Path: Invalid arguments to cache_get_and_retain */
    ASSERT_TRUE(cache_get_and_retain(NULL, "primary_key", &value_out) == EINVAL,
        "cache_get_and_retain with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_get_and_retain(cache, NULL, &value_out) == EINVAL,
        "cache_get_and_retain with NULL key must return EINVAL");
    ASSERT_TRUE(cache_get_and_retain(cache, "primary_key", NULL) == EINVAL,
        "cache_get_and_retain with NULL value_out must return EINVAL");

    /* Negative Path: Invalid arguments and unknown values in cache_release_value */
    ASSERT_TRUE(cache_release_value(NULL, replacement_payload) == EINVAL,
        "cache_release_value with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_release_value(cache, NULL) == 0,
        "cache_release_value with NULL value must return 0");
    ASSERT_TRUE(cache_release_value(cache, "untracked_value_ptr") == ENOENT,
        "cache_release_value for an untracked value pointer must return ENOENT");

    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy must succeed");
}

/*
 * Validates cache_remove (including deferred finalization when retained)
 * and cache_remove_all defined in cache.h.
 */
void
test_cache_remove_and_remove_all(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    callback_tracker_t tracker;
    void *value_out = NULL;
    int status;

    memset(&tracker, 0, sizeof(tracker));
    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    attrs.key_hash_cb = cache_key_hash_cb_cstring;
    attrs.key_is_equal_cb = cache_key_is_equal_cb_cstring;
    attrs.key_retain_cb = custom_key_retain_cb;
    attrs.key_release_cb = custom_key_release_cb;
    attrs.value_release_cb = custom_value_release_cb;
    attrs.user_data = &tracker;

    status = cache_create("com.darwin.test.remove", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create must succeed");

    /* Negative Path: Invalid arguments to cache_remove and cache_remove_all */
    ASSERT_TRUE(cache_remove(NULL, "key") == EINVAL,
        "cache_remove with NULL cache must return EINVAL");
    ASSERT_TRUE(cache_remove(cache, NULL) == EINVAL,
        "cache_remove with NULL key must return EINVAL");
    ASSERT_TRUE(cache_remove(cache, "missing_key") == ENOENT,
        "cache_remove for a missing key must return ENOENT");
    ASSERT_TRUE(cache_remove_all(NULL) == EINVAL,
        "cache_remove_all with NULL cache must return EINVAL");

    /* Positive Path: cache_remove_all on an empty cache succeeds */
    ASSERT_TRUE(cache_remove_all(cache) == 0,
        "cache_remove_all on an empty cache must return 0");

    /* Populate cache with three retained entries */
    char *val1 = strdup("value_one");
    char *val2 = strdup("value_two");
    char *val3 = strdup("value_three");

    ASSERT_TRUE(cache_set_and_retain(cache, "key_1", val1, 10) == 0,
        "Inserting key_1 must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "key_2", val2, 20) == 0,
        "Inserting key_2 must succeed");
    ASSERT_TRUE(cache_set_and_retain(cache, "key_3", val3, 30) == 0,
        "Inserting key_3 must succeed");

    /* Release key_1 and key_3 so they have retain_count == 0, keep key_2 retained */
    ASSERT_TRUE(cache_release_value(cache, val1) == 0,
        "Releasing val1 must succeed");
    ASSERT_TRUE(cache_release_value(cache, val3) == 0,
        "Releasing val3 must succeed");

    /*
     * Positive Path: Removing key_2 while still retained (retain_count == 1)
     * hides it from cache_get_and_retain immediately, but defers value_release_cb
     * until cache_release_value(cache, val2) is called.
     */
    status = cache_remove(cache, "key_2");
    ASSERT_TRUE(status == 0,
        "cache_remove for retained key_2 must return 0");
    ASSERT_TRUE(cache_get_and_retain(cache, "key_2", &value_out) == ENOENT,
        "Removed key_2 must immediately return ENOENT on lookup");
    ASSERT_TRUE(tracker.value_release_calls == 0,
        "Retained value must not be finalized before cache_release_value is called");

    status = cache_release_value(cache, val2);
    ASSERT_TRUE(status == 0,
        "Releasing last reference to removed value must return 0");
    ASSERT_TRUE(tracker.value_release_calls == 1,
        "Value must be finalized as soon as the last reference is released");

    /* Positive Path: Removing unretained key_1 finalizes it immediately */
    status = cache_remove(cache, "key_1");
    ASSERT_TRUE(status == 0,
        "cache_remove for unretained key_1 must return 0");
    ASSERT_TRUE(tracker.value_release_calls == 2,
        "Unretained entry must be finalized immediately upon cache_remove");

    /* Positive Path: cache_remove_all flushes remaining entries and defers retained ones */
    char *val4 = strdup("value_four_retained");
    ASSERT_TRUE(cache_set_and_retain(cache, "key_4", val4, 40) == 0,
        "Inserting key_4 must succeed");

    status = cache_remove_all(cache);
    ASSERT_TRUE(status == 0,
        "cache_remove_all on a populated cache must return 0");
    ASSERT_TRUE(tracker.value_release_calls == 3,
        "cache_remove_all must finalize unretained entries while deferring retained entries");
    ASSERT_TRUE(cache_get_and_retain(cache, "key_3", &value_out) == ENOENT,
        "key_3 must return ENOENT after cache_remove_all");
    ASSERT_TRUE(cache_get_and_retain(cache, "key_4", &value_out) == ENOENT,
        "Retained key_4 must also return ENOENT after cache_remove_all");
    ASSERT_TRUE(cache_release_value(cache, val4) == 0,
        "Releasing retained value after cache_remove_all must return 0");
    ASSERT_TRUE(tracker.value_release_calls == 4,
        "Deferred entry from cache_remove_all must be finalized upon cache_release_value");

    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy must succeed");
}

/*
 * Validates CACHE_ATTRIBUTES_VERSION_1 callback invocations including
 * key_hash_cb, key_is_equal_cb, key_retain_cb, key_release_cb,
 * value_release_cb, value_make_purgeable_cb, value_make_nonpurgeable_cb,
 * and user_data context forwarding.
 */
void
test_cache_custom_callbacks_v1(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    callback_tracker_t tracker;
    void *value_out = NULL;
    int status;

    memset(&tracker, 0, sizeof(tracker));

    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_1;
    attrs.key_hash_cb = custom_key_hash_cb;
    attrs.key_is_equal_cb = custom_key_is_equal_cb;
    attrs.key_retain_cb = custom_key_retain_cb;
    attrs.key_release_cb = custom_key_release_cb;
    attrs.value_release_cb = custom_value_release_cb;
    attrs.value_make_purgeable_cb = custom_value_make_purgeable_cb;
    attrs.value_make_nonpurgeable_cb = custom_value_make_nonpurgeable_cb;
    attrs.value_retain_cb = custom_value_retain_cb; /* Must be ignored in VERSION_1 */
    attrs.user_data = &tracker;

    status = cache_create("com.darwin.test.custom.v1", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create with VERSION_1 custom callbacks must succeed");

    /* Stack-allocated key buffer to verify key_retain_cb duplicates the key */
    char transient_key[32];
    snprintf(transient_key, sizeof(transient_key), "v1_dynamic_key");
    char *heap_val = strdup("v1_dynamic_value");

    status = cache_set_and_retain(cache, transient_key, heap_val, 50);
    ASSERT_TRUE(status == 0,
        "cache_set_and_retain must succeed with VERSION_1 key_retain_cb");
    ASSERT_TRUE(tracker.hash_calls >= 1,
        "key_hash_cb must be invoked with user_data during cache_set_and_retain");
    ASSERT_TRUE(tracker.key_retain_calls == 1,
        "VERSION_1 key_retain_cb must be invoked once on insertion");
    ASSERT_TRUE(tracker.value_retain_calls == 0,
        "VERSION_1 must ignore value_retain_cb");

    /* Overwrite stack buffer to prove cache retained its own copy of the key */
    memset(transient_key, 'X', sizeof(transient_key) - 1);

    /* Release initial hold so retain_count drops to 0 and triggers value_make_purgeable_cb */
    ASSERT_TRUE(cache_release_value(cache, heap_val) == 0,
        "Releasing initial set reference must succeed");
    ASSERT_TRUE(tracker.make_purgeable_calls == 1,
        "value_make_purgeable_cb must be invoked when retain count drops to 0");

    /* Re-acquire entry from retain_count == 0 to trigger value_make_nonpurgeable_cb */
    status = cache_get_and_retain(cache, "v1_dynamic_key", &value_out);
    ASSERT_TRUE(status == 0 && value_out == heap_val,
        "Retained key copy must match lookup even after caller mutates original key buffer");
    ASSERT_TRUE(tracker.equal_calls >= 1,
        "key_is_equal_cb must be invoked with user_data during cache_get_and_retain");
    ASSERT_TRUE(tracker.make_nonpurgeable_calls == 1,
        "value_make_nonpurgeable_cb must be invoked when retaining an unretained entry");

    /* Release back to 0 and simulate kernel purge on next lookup */
    ASSERT_TRUE(cache_release_value(cache, value_out) == 0,
        "Releasing lookup reference must succeed");
    ASSERT_TRUE(tracker.make_purgeable_calls == 2,
        "value_make_purgeable_cb must be invoked again when retain count drops to 0");

    tracker.simulate_purged = true;
    status = cache_get_and_retain(cache, "v1_dynamic_key", &value_out);
    ASSERT_TRUE(status == ENOENT,
        "cache_get_and_retain must evict and return ENOENT when value_make_nonpurgeable_cb returns false");
    ASSERT_TRUE(value_out == NULL,
        "cache_get_and_retain must set *value_out to NULL when entry is purged");
    ASSERT_TRUE(tracker.key_release_calls == 1,
        "key_release_cb must be invoked when purged entry is evicted");
    ASSERT_TRUE(tracker.value_release_calls == 1,
        "value_release_cb must be invoked when purged entry is evicted");

    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy must succeed");
}

/*
 * Validates CACHE_ATTRIBUTES_VERSION_2 callback invocations including
 * value_retain_cb and cache_release_cb_free.
 */
void
test_cache_custom_callbacks_v2(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    callback_tracker_t tracker;
    void *value_out = NULL;
    int status;

    memset(&tracker, 0, sizeof(tracker));

    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    attrs.key_hash_cb = custom_key_hash_cb;
    attrs.key_is_equal_cb = custom_key_is_equal_cb;
    attrs.key_retain_cb = custom_key_retain_cb;
    attrs.key_release_cb = custom_key_release_cb;
    attrs.value_retain_cb = custom_value_retain_cb;
    attrs.value_release_cb = custom_value_release_cb;
    attrs.user_data = &tracker;

    status = cache_create("com.darwin.test.custom.v2", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create with VERSION_2 custom callbacks must succeed");

    char *heap_val = strdup("v2_heap_value");
    ASSERT_TRUE(heap_val != NULL, "Value allocation must succeed");

    status = cache_set_and_retain(cache, "v2_key", heap_val, 25);
    ASSERT_TRUE(status == 0,
        "cache_set_and_retain with VERSION_2 callbacks must succeed");
    ASSERT_TRUE(tracker.key_retain_calls == 1,
        "VERSION_2 key_retain_cb must be invoked once on insertion");
    ASSERT_TRUE(tracker.value_retain_calls == 1,
        "VERSION_2 value_retain_cb must be invoked once on insertion");

    status = cache_get_and_retain(cache, "v2_key", &value_out);
    ASSERT_TRUE(status == 0 && value_out == heap_val,
        "cache_get_and_retain must retrieve VERSION_2 value");

    ASSERT_TRUE(cache_release_value(cache, value_out) == 0,
        "Releasing lookup hold on VERSION_2 value must succeed");
    ASSERT_TRUE(cache_release_value(cache, value_out) == 0,
        "Releasing initial hold on VERSION_2 value must succeed");

    /* Destroy cache and confirm remaining entry triggers key and value release callbacks */
    status = cache_destroy(cache);
    ASSERT_TRUE(status == 0,
        "cache_destroy must succeed");
    ASSERT_TRUE(tracker.key_release_calls == 1,
        "key_release_cb must be invoked during cache_destroy");
    ASSERT_TRUE(tracker.value_release_calls == 1,
        "value_release_cb must be invoked during cache_destroy");
}

/*
 * Validates integer key callbacks integrated with cache_t and tests
 * multi-element insertions across table rehash expansions.
 */
void
test_cache_scaling_and_integer_keys(void)
{
    cache_t *cache = NULL;
    cache_attributes_t attrs;
    int status;
    uintptr_t i;

    memset(&attrs, 0, sizeof(attrs));
    attrs.version = CACHE_ATTRIBUTES_VERSION_2;
    attrs.key_hash_cb = cache_key_hash_cb_integer;
    attrs.key_is_equal_cb = cache_key_is_equal_cb_integer;
    attrs.value_release_cb = cache_release_cb_free;

    status = cache_create("com.darwin.test.integer_scaling", &attrs, &cache);
    ASSERT_TRUE(status == 0 && cache != NULL,
        "cache_create with integer key callbacks must succeed");

    /* Insert 64 integer-keyed entries to exercise table bucket expansion */
    for (i = 1; i <= 64; i++) {
        char *val = malloc(32);
        ASSERT_TRUE(val != NULL, "Value buffer allocation must succeed");
        snprintf(val, 32, "int_val_%lu", (unsigned long)i);

        status = cache_set_and_retain(cache, (void *)i, val, 16);
        ASSERT_TRUE(status == 0,
            "cache_set_and_retain for integer key during scaling must return 0");
        ASSERT_TRUE(cache_release_value(cache, val) == 0,
            "cache_release_value after integer key insertion must return 0");
    }

    /* Verify all 64 entries remain accessible after rehashing */
    for (i = 1; i <= 64; i++) {
        void *fetched = NULL;
        char expected[32];
        snprintf(expected, sizeof(expected), "int_val_%lu", (unsigned long)i);

        status = cache_get_and_retain(cache, (void *)i, &fetched);
        ASSERT_TRUE(status == 0 && fetched != NULL,
            "All integer keys must be retrievable after bucket expansion");
        ASSERT_TRUE(strcmp((const char *)fetched, expected) == 0,
            "Retrieved value for integer key must match expected payload");
        ASSERT_TRUE(cache_release_value(cache, fetched) == 0,
            "Releasing fetched integer-keyed value must return 0");
    }

    /* Remove half of the keys explicitly and verify removal */
    for (i = 1; i <= 32; i++) {
        void *fetched = NULL;
        ASSERT_TRUE(cache_remove(cache, (void *)i) == 0,
            "cache_remove on existing integer key must return 0");
        ASSERT_TRUE(cache_get_and_retain(cache, (void *)i, &fetched) == ENOENT,
            "Removed integer key must return ENOENT on subsequent lookup");
    }

    /* Flush remaining entries and destroy */
    ASSERT_TRUE(cache_remove_all(cache) == 0,
        "cache_remove_all on remaining integer keys must return 0");
    ASSERT_TRUE(cache_destroy(cache) == 0,
        "cache_destroy must return 0");
}
