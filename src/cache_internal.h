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

/* Dispatch queue label for memory pressure monitoring (debug builds only). */
#if DEBUG
#define CACHE_MONITOR_QUEUE_LABEL "com.darwin.libcache.monitor"
#else
#define CACHE_MONITOR_QUEUE_LABEL NULL
#endif

/* Maximum number of hash table buckets permitted during rehash expansion. */
#define CACHE_MAX_BUCKET_COUNT 65536

/* Baseline default physical memory size (1 GB) used when sysctl probing fails. */
#define CACHE_DEFAULT_PHYSICAL_MEMORY_BYTES (1024ULL * 1024 * 1024)

/* Memory scaling denominator: allocates 1 cache entry per 64 KB of RAM (~16 entries/MB). */
#define CACHE_BYTES_PER_DYNAMIC_ENTRY (64 * 1024)

/* Minimum dynamic capacity limit for newly created cache instances. */
#define CACHE_MIN_DYNAMIC_CAPACITY_LIMIT 1024

/* Minimum capacity limit floor when operating under WARNING memory pressure. */
#define CACHE_MIN_WARN_CAPACITY_LIMIT 128

/* Minimum capacity limit floor when operating under CRITICAL memory pressure. */
#define CACHE_MIN_CRITICAL_CAPACITY_LIMIT 64

/* Target CPU cache line size to prevent false sharing under high concurrency. */
#define CACHE_LINE_SIZE 64

/* Cache-line padded stripe lock preventing false sharing across concurrent buckets. */
typedef struct {
    os_unfair_lock lock;
    uint8_t        pad[CACHE_LINE_SIZE - sizeof(os_unfair_lock)];
} cache_stripe_lock_t;

/* Maps a key hash value to its stripe lock pointer using stripe_mask. */
#define GET_STRIPE_LOCK(cache, hash_val) \
    (&((cache)->locks[(hash_val) & (cache)->stripe_mask].lock))

/* Maps a key hash value to its key hash bucket head using bucket_mask. */
#define GET_KEY_BUCKET(cache, hash_val) \
    ((cache)->buckets[(hash_val) & (cache)->bucket_mask].key_head)

/* Maps a value hash value to its value-pointer hash bucket head using bucket_mask. */
#define GET_VAL_BUCKET(cache, hash_val) \
    ((cache)->buckets[(hash_val) & (cache)->bucket_mask].val_head)

/* Maps a value hash value to its stripe lock pointer using stripe_mask. */
#define GET_VAL_STRIPE_LOCK(cache, val_hash) \
    (&((cache)->val_locks[(val_hash) & (cache)->stripe_mask].lock))

/* Computes the hash value for a key using the configured key_hash_cb. */
#define CACHE_KEY_HASH(cache, key) \
    ((cache)->attrs.key_hash_cb((key), (cache)->attrs.user_data))

/* Evaluates whether a cache entry matches the specified key and hash value.
 * Uses pointer equality shortcut to bypass callback for identical/integer keys. */
#define CACHE_KEY_MATCH(cache, entry, key, hash_val) \
    ((entry)->key_hash == (hash_val) && \
     ((entry)->key == (key) || \
      (cache)->attrs.key_is_equal_cb((entry)->key, (key), \
      (cache)->attrs.user_data)))

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
    uintptr_t v = (uintptr_t)ptr >> 4;
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    return ((size_t)v);
}

/* Cache entry node stored in the key hash table, value index, and SLRU segments. */
typedef struct cache_entry_s {
    void                    *key;          /* Entry key pointer. */
    void                    *value;        /* Entry value pointer. */
    uintptr_t                key_hash;     /* Cached key hash value. */
    cache_cost_t             cost;         /* Entry cost weight. */
    _Atomic int              retain_count; /* Reference count; prevents eviction when > 0. */
    bool                     is_removed;   /* True if entry was unlinked from cache while still retained. */
    bool                     is_protected; /* True if entry is in protected segment; false if probationary. */
    struct cache_entry_s    *next;         /* Next entry in key bucket or free list. */
    struct cache_entry_s    *val_next;     /* Next entry in value-pointer bucket chain. */
    struct cache_entry_s    *lru_next;     /* Next less-recently-used entry toward segment tail. */
    struct cache_entry_s    *lru_prev;     /* Previous more-recently-used entry toward segment head. */
} cache_entry_t;

/* Bucket slot storing independent heads for key hash chains and value hash chains. */
typedef struct cache_bucket_s {
    cache_entry_t          *key_head;              /* Head of key hash collision chain. */
    cache_entry_t          *val_head;              /* Head of value-pointer hash collision chain. */
} cache_bucket_t;

/* Core cache container structure. */
struct cache_s {
    char                    *name;                   /* Descriptive cache name string. */
    cache_stripe_lock_t     *locks;                  /* Array of cache-line padded stripe locks for key buckets. */
    cache_stripe_lock_t     *val_locks;              /* Array of cache-line padded stripe locks for value buckets. */
    size_t                   num_stripes;            /* Number of stripe locks (power of two). */
    size_t                   stripe_mask;            /* Bitwise stripe index mask (num_stripes - 1). */
    os_unfair_lock           lru_lock;               /* Lock protecting SLRU queues. */
    cache_attributes_t       attrs;                  /* Configured cache attributes and callbacks. */
    _Atomic cache_cost_t     current_cost;           /* Aggregate cost of all active entries. */
    _Atomic size_t           current_count;          /* Total number of active entries. */
    _Atomic size_t           minimum_values_hint;    /* Configured minimum values hint. */
    _Atomic size_t           configured_count_hint;  /* Configured maximum entry count ceiling (0 = automatic). */
    _Atomic size_t           dynamic_capacity_limit; /* Dynamic memory-proportional capacity limit. */
    size_t                   base_capacity_limit;    /* Baseline dynamic capacity limit at normal memory. */
    _Atomic uint32_t         user_ref_count;         /* Reference count for user descriptor handle. */
    _Atomic uint32_t         ref_count;              /* Reference count for cache descriptor lifecycle. */
    size_t                   total_retained_entries; /* Total active retained entries; protected by lru_lock. */
    _Atomic bool             deferred_destroy;       /* True if cache_release was called while entries were retained. */
    malloc_zone_t           *memory_zone;            /* Backing malloc zone for cache_entry_t allocations. */
    dispatch_queue_t         monitor_queue;          /* Serial dispatch queue for memory-pressure events. */
    dispatch_source_t        mem_pressure_source;    /* GCD memory-pressure event source. */
    dispatch_source_t        sim_pressure_source;    /* GCD simulated memory-pressure event source. */
    struct cache_s          *sim_next;               /* Next active cache in simulated pressure list. */
    _Atomic bool             is_destroying;          /* Flag indicating teardown is in progress. */
    cache_bucket_t          *buckets;                /* Unified key and value hash table bucket array. */
    size_t                   bucket_count;           /* Number of hash buckets (power of two). */
    size_t                   bucket_mask;            /* Bitwise bucket mask (bucket_count - 1). */
    cache_entry_t           *prob_head;              /* MRU entry at head of probationary segment. */
    cache_entry_t           *prob_tail;              /* LRU entry at tail of probationary segment. */
    size_t                   prob_count;             /* Count of active entries in probationary segment. */
    cache_entry_t           *prot_head;              /* MRU entry at head of protected segment. */
    cache_entry_t           *prot_tail;              /* LRU entry at tail of protected segment. */
    size_t                   prot_count;             /* Count of active entries in protected segment. */
    cache_entry_t           *head;                   /* Overall MRU entry alias. */
    cache_entry_t           *tail;                   /* Overall LRU entry alias. */
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
        os_unfair_lock_lock(&cache->locks[i].lock);
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
        os_unfair_lock_unlock(&cache->locks[i].lock);
}

/**
 * @brief Acquires all value stripe locks in ascending index order.
 * @param cache A valid cache instance.
 */
static inline void
cache_lock_val_stripes(cache_t *cache)
{
    ssize_t i;

    for (i = 0; i < (ssize_t)cache->num_stripes; i++)
        os_unfair_lock_lock(&cache->val_locks[i].lock);
}

/**
 * @brief Releases all value stripe locks in descending index order.
 * @param cache A valid cache instance.
 */
static inline void
cache_unlock_val_stripes(cache_t *cache)
{
    ssize_t i;

    for (i = (ssize_t)cache->num_stripes - 1; i >= 0; i--)
        os_unfair_lock_unlock(&cache->val_locks[i].lock);
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

/**
 * @brief Returns the effective entry count limit for the cache.
 * @details If a manual count hint is configured by the caller (configured_count_hint > 0),
 *          that manual hint takes precedence. Otherwise, the dynamic memory-proportional
 *          capacity limit is used so the framework automatically controls cache size.
 * @param cache A valid cache instance.
 * @return Effective entry count ceiling.
 */
size_t cache_get_effective_count_limit(const cache_t *cache);

/* Internal helper functions for SLRU queues, value index, and eviction operations. */
void detach(cache_t *cache, cache_entry_t *entry);
void attach_probation_head(cache_t *cache, cache_entry_t *entry);
void attach_protected_head(cache_t *cache, cache_entry_t *entry);
void touch_entry(cache_t *cache, cache_entry_t *entry);
void attach_val_bucket(cache_t *cache, cache_entry_t *entry);
void detach_val_bucket(cache_t *cache, cache_entry_t *entry);
void detach_key_bucket(cache_t *cache, cache_entry_t *entry);
void mark_entry_removed(cache_t *cache, cache_entry_t *entry);
void free_entry_resources(cache_t *cache, cache_entry_t *entry);
void detach_entry(cache_t *cache, cache_entry_t *entry);
void cache_init_memory_pressure_monitor(cache_t *cache);
void cache_teardown_memory_pressure_monitor(cache_t *cache);

#endif /* !_CACHE_INTERNAL_H_ */
