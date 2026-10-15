/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#ifndef _CACHE_INTERNAL_H_
#define _CACHE_INTERNAL_H_

#include <os/lock.h>
#include <malloc/malloc.h>
#include <stdatomic.h>
#include <dispatch/dispatch.h>

#include "cache.h"
#include "cache_private.h"
#include "cache_callbacks.h"

/* Maximum number of hash table buckets permitted during rehash expansion. */
#define CACHE_MAX_BUCKET_COUNT 65536

/* Maps a key hash value to its stripe lock pointer using stripe_mask. */
#define GET_STRIPE_LOCK(cache, hash_val) \
    (&((cache)->locks[(hash_val) & (cache)->stripe_mask]))

/* Maps a key hash value to its key hash bucket head using bucket_mask. */
#define GET_KEY_BUCKET(cache, hash_val) \
    ((cache)->buckets[(hash_val) & (cache)->bucket_mask])

/* Computes the hash value for a key using the configured key_hash_cb. */
#define CACHE_KEY_HASH(cache, key) \
    ((cache)->attrs.key_hash_cb((key), (cache)->attrs.user_data))

/* Evaluates whether a cache entry matches the specified key and hash value. */
#define CACHE_KEY_MATCH(cache, entry, key, hash_val) \
    ((entry)->key_hash == (hash_val) && \
     (cache)->attrs.key_is_equal_cb((entry)->key, (key), \
     (cache)->attrs.user_data))

/**
 * @brief Computes a 64-bit hash mix of a pointer address for O(1)
 *        value-bucket indexing.
 * @details Uses MurmurHash3 to compute the hash.
 * @param ptr Pointer address to hash.
 * @return Mixed unsigned integer hash value derived from the pointer address.
 */
static inline size_t
cache_hash_pointer(const void *ptr)
{
    uintptr_t v = (uintptr_t)ptr;
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    return ((size_t)v);
}

/* Cache entry node stored in the key hash table, value index, and LRU list. */
typedef struct cache_entry_s {
    void                    *key;          /* Entry key pointer. */
    void                    *value;        /* Entry value pointer. */
    uintptr_t                key_hash;     /* Cached key hash value. */
    cache_cost_t             cost;         /* Entry cost weight. */
    int                      retain_count; /* Reference count; prevents eviction when > 0. */
    struct cache_entry_s    *next;         /* Next entry in key bucket, removed_head, or free list. */
    struct cache_entry_s    *val_next;     /* Next entry in value-pointer bucket chain. */
    struct cache_entry_s    *lru_next;     /* Next less-recently-used entry toward LRU tail. */
    struct cache_entry_s    *lru_prev;     /* Previous more-recently-used entry toward MRU head. */
} cache_entry_t;

/* Core cache container structure. */
struct cache_s {
    char                    *name;                  /* Descriptive cache name string. */
    os_unfair_lock          *locks;                 /* Array of stripe locks protecting key buckets. */
    size_t                   num_stripes;           /* Number of stripe locks (power of two). */
    size_t                   stripe_mask;           /* Bitwise stripe index mask (num_stripes - 1). */
    os_unfair_lock           lru_lock;              /* Lock protecting LRU list, value index, and removed list. */
    cache_attributes_t       attrs;                 /* Configured cache attributes and callbacks. */
    _Atomic cache_cost_t     current_cost;          /* Aggregate cost of all active entries. */
    _Atomic size_t           current_count;         /* Total number of active entries. */
    _Atomic size_t           minimum_values_hint;   /* Configured minimum values hint. */
    _Atomic size_t           configured_count_hint; /* Configured maximum entry count ceiling. */
    _Atomic uint32_t         ref_count;             /* Reference count for cache descriptor lifecycle. */
    malloc_zone_t           *memory_zone;           /* Backing malloc zone for cache_entry_t allocations. */
    dispatch_queue_t         monitor_queue;         /* Serial dispatch queue for memory-pressure events. */
    dispatch_source_t        mem_pressure_source;   /* GCD memory-pressure event source. */
    dispatch_source_t        sim_pressure_source;   /* GCD simulated memory-pressure event source. */
    struct cache_s          *sim_next;              /* Next active cache in simulated pressure list. */
    _Atomic bool             is_destroying;         /* Flag indicating teardown is in progress. */
    cache_entry_t          **buckets;               /* Key hash table bucket array. */
    size_t                   bucket_count;          /* Number of key hash buckets (power of two). */
    size_t                   bucket_mask;           /* Bitwise key bucket mask (bucket_count - 1). */
    cache_entry_t          **val_buckets;           /* Value-pointer hash table bucket array. */
    size_t                   val_bucket_count;      /* Number of value-pointer hash buckets (power of two). */
    size_t                   val_bucket_mask;       /* Bitwise value bucket mask (val_bucket_count - 1). */
    cache_entry_t           *head;                  /* Most-recently-used (MRU) entry at list head. */
    cache_entry_t           *tail;                  /* Least-recently-used (LRU) entry at list tail. */
    cache_entry_t           *removed_head;          /* Singly linked list of removed entries still retained. */
};

/**
 * @brief Acquires all stripe locks in ascending index order.
 * @param cache A valid cache instance.
 */
static inline void
cache_lock_stripes(cache_t *cache)
{
    ssize_t i;

    for (i = 0; i < (ssize_t)cache->num_stripes; i++)
        os_unfair_lock_lock(&cache->locks[i]);
}

/**
 * @brief Releases all stripe locks in descending index order.
 * @param cache A valid cache instance.
 */
static inline void
cache_unlock_stripes(cache_t *cache)
{
    ssize_t i;

    for (i = (ssize_t)cache->num_stripes - 1; i >= 0; i--)
        os_unfair_lock_unlock(&cache->locks[i]);
}

/**
 * @brief Acquires the cache LRU lock.
 * @param cache A valid cache instance.
 */
static inline void
cache_lock_lru(cache_t *cache)
{
    os_unfair_lock_lock(&cache->lru_lock);
}

/**
 * @brief Releases the cache LRU lock.
 * @param cache A valid cache instance.
 */
static inline void
cache_unlock_lru(cache_t *cache)
{
    os_unfair_lock_unlock(&cache->lru_lock);
}

/* Internal helper functions for LRU list, value index, and eviction operations. */
void detach_lru(cache_t *cache, cache_entry_t *entry);
void attach_lru_head(cache_t *cache, cache_entry_t *entry);
void attach_val_bucket(cache_t *cache, cache_entry_t *entry);
void detach_val_bucket(cache_t *cache, cache_entry_t *entry);
void free_entry_resources(cache_t *cache, cache_entry_t *entry);
void detach_entry(cache_t *cache, cache_entry_t *entry);

int _cache_handle_memory_pressure_event(cache_t *cache, dispatch_source_memorypressure_flags_t flags);
void _cache_sim_pressure_enroll(cache_t *cache);
void _cache_sim_pressure_unenroll(cache_t *cache);

#endif /* !_CACHE_INTERNAL_H_ */
