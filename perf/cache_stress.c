/**
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 *
 * cache_stress.c
 *
 * High-Concurrency Darwin libcache Stress Test & Invariant Verifier
 *
 * Exercises Apple's public <cache.h> and <cache_callbacks.h> APIs under heavy
 * multi-threaded insertion, retrieval, partition eviction sweeps, and Darwin
 * kernel memory pressure (DISPATCH_SOURCE_TYPE_MEMORYPRESSURE + Mach VM
 * purgeable pages).
 *
 * Run with configured profile:
 *   ./cache_stress --threads 12 --duration 10 --keys 2048 --ballast 32 --zipf --mach-purgeable
 *
 * Run under macOS leaks detector:
 *   MallocStackLogging=1 leaks --atExit -- ./cache_stress --threads 12 --duration 10 --keys 2048 --ballast 32 --zipf --mach-purgeable
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>

#include <cache.h>
#include <cache_callbacks.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_purgable.h>
#include <mach/mach_time.h>

/* ========================================================================== *
 * Constants & Canary Signatures                                              *
 * ========================================================================== */

#define KEY_MAGIC_LIVE          0x4B45595F4C495645ULL /* "KEY_LIVE" */
#define KEY_MAGIC_FREED         0x4B45595F44454144ULL /* "KEY_DEAD" */
#define VALUE_MAGIC_LIVE        0xCAFEBABE5A5A5A5AULL
#define VALUE_MAGIC_FREED       0xDEADFEEDDEADFEEDULL
#define FNV1A_64_OFFSET_BASIS   0xcbf29ce484222325ULL
#define FNV1A_64_PRIME          0x00000100000001b3ULL

/* ========================================================================== *
 * Types & Telemetry Context                                                  *
 * ========================================================================== */

typedef enum {
    PRESSURE_LEVEL_NORMAL   = 0,
    PRESSURE_LEVEL_WARN     = 1,
    PRESSURE_LEVEL_CRITICAL = 2
} pressure_level_t;

typedef struct {
    uint64_t magic;
    uint64_t key_id;
    char     label[32];
} stress_key_t;

typedef struct {
    uint64_t            magic_header;
    uint64_t            key_id;
    uint64_t            generation;
    _Atomic int32_t     active_readers;
    _Atomic bool        is_purgeable;
    _Atomic bool        was_purged_by_kernel;
    size_t              cost;
    size_t              payload_size;
    uint64_t            checksum;
    mach_vm_address_t   vm_page_addr;
    mach_vm_size_t      vm_page_size;
    uint8_t             payload[];
} stress_value_t;

typedef struct {
    /* Operation counters */
    _Atomic uint64_t insertions_total;
    _Atomic uint64_t gets_hit;
    _Atomic uint64_t gets_miss;
    _Atomic uint64_t explicit_removes;
    _Atomic uint64_t partition_sweeps;
    _Atomic uint64_t full_purges;
    _Atomic uint64_t probe_sweeps;

    /* Callback lifecycle counters */
    _Atomic uint64_t hash_callbacks;
    _Atomic uint64_t equal_callbacks;
    _Atomic uint64_t keys_retained;
    _Atomic uint64_t keys_released;
    _Atomic uint64_t values_allocated;
    _Atomic uint64_t values_released;
    _Atomic uint64_t make_purgeable_calls;
    _Atomic uint64_t make_nonpurgeable_calls;
    _Atomic uint64_t volatile_emptied_events;

    /* Memory pressure counters */
    _Atomic uint64_t os_pressure_events;
    _Atomic uint64_t synthetic_pressure_waves;
    _Atomic int      current_pressure_level;

    /* Integrity & latency tracking */
    _Atomic uint64_t invariant_violations;
    _Atomic uint64_t checksum_verifications;
    _Atomic uint64_t total_get_latency_ns;
    _Atomic uint64_t total_set_latency_ns;
    _Atomic uint64_t max_get_latency_ns;
} stress_stats_t;

typedef struct {
    uint32_t num_threads;
    uint32_t duration_sec;
    uint64_t key_space;
    size_t   min_payload_bytes;
    size_t   max_payload_bytes;
    size_t   pressure_ballast_mb;
    bool     use_zipfian;
    bool     use_mach_purgeable;
    bool     verbose;
} stress_config_t;

typedef struct {
    cache_t                  *cache;
    stress_config_t           cfg;
    stress_stats_t            stats;
    _Atomic bool              stop_flag;
    dispatch_source_t         memory_pressure_source;
    mach_timebase_info_data_t timebase;
} stress_context_t;

typedef struct {
    stress_context_t *ctx;
    uint32_t          thread_id;
    uint64_t          rng_state;
} worker_arg_t;

static volatile sig_atomic_t g_interrupted = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

/* ========================================================================== *
 * Fast PRNG (xorshift64*) & FNV-1a Checksum                                  *
 * ========================================================================== */

static inline uint64_t xorshift64s(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static uint64_t compute_payload_checksum(uint64_t key_id,
                                         uint64_t generation,
                                         const uint8_t *payload,
                                         size_t len) {
    uint64_t hash = FNV1A_64_OFFSET_BASIS;
    hash ^= key_id;
    hash *= FNV1A_64_PRIME;
    hash ^= generation;
    hash *= FNV1A_64_PRIME;

    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)payload[i];
        hash *= FNV1A_64_PRIME;
    }
    return hash;
}

static inline uint64_t mach_ticks_to_ns(uint64_t ticks,
                                        const mach_timebase_info_data_t *tb) {
    return (ticks * tb->numer) / tb->denom;
}

static inline void update_max_u64(_Atomic uint64_t *target, uint64_t val) {
    uint64_t prev = atomic_load_explicit(target, memory_order_relaxed);
    while (val > prev &&
           !atomic_compare_exchange_weak_explicit(
               target, &prev, val, memory_order_relaxed, memory_order_relaxed)) {
    }
}

/*
 * Zipfian key selector: concentrates ~65% of accesses onto the hottest 10%
 * of keys to maximize concurrent retain/release contention inside libcache.
 */
static uint64_t select_key_id(uint64_t *rng, uint64_t key_space, bool zipfian) {
    uint64_t r = xorshift64s(rng);
    if (!zipfian || key_space < 16) {
        return r % key_space;
    }
    uint64_t bucket_roll = r % 100;
    uint64_t hot_zone = key_space / 10 > 0 ? key_space / 10 : 1;
    uint64_t warm_zone = key_space / 3 > 0 ? key_space / 3 : 1;

    if (bucket_roll < 65) {
        return (xorshift64s(rng) % hot_zone);
    } else if (bucket_roll < 90) {
        return hot_zone + (xorshift64s(rng) % warm_zone);
    } else {
        return xorshift64s(rng) % key_space;
    }
}

/* ========================================================================== *
 * libcache Callbacks (CACHE_ATTRIBUTES_VERSION_2)                            *
 * ========================================================================== */

static uintptr_t stress_key_hash_cb(void *key, void *user_data) {
    stress_context_t *ctx = (stress_context_t *)user_data;
    const stress_key_t *k = (const stress_key_t *)key;

    if (!k || k->magic != KEY_MAGIC_LIVE) {
        atomic_fetch_add_explicit(&ctx->stats.invariant_violations, 1, memory_order_relaxed);
        fprintf(stderr, "[FATAL INVARIANT] key_hash_cb invoked on invalid key %p\n", key);
        return 0;
    }

    atomic_fetch_add_explicit(&ctx->stats.hash_callbacks, 1, memory_order_relaxed);

    /* MurmurHash3 64-bit finalizer mix */
    uint64_t h = k->key_id ^ 0x9E3779B97F4A7C15ULL;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (uintptr_t)h;
}

static bool stress_key_is_equal_cb(void *key1, void *key2, void *user_data) {
    stress_context_t *ctx = (stress_context_t *)user_data;
    const stress_key_t *k1 = (const stress_key_t *)key1;
    const stress_key_t *k2 = (const stress_key_t *)key2;

    atomic_fetch_add_explicit(&ctx->stats.equal_callbacks, 1, memory_order_relaxed);

    if (!k1 || !k2 || k1->magic != KEY_MAGIC_LIVE || k2->magic != KEY_MAGIC_LIVE) {
        atomic_fetch_add_explicit(&ctx->stats.invariant_violations, 1, memory_order_relaxed);
        fprintf(stderr, "[FATAL INVARIANT] key_is_equal_cb encountered corrupt key magic\n");
        return false;
    }

    return k1->key_id == k2->key_id;
}

static void stress_key_retain_cb(void *key_in, void **key_out, void *user_data) {
    stress_context_t *ctx = (stress_context_t *)user_data;
    const stress_key_t *src = (const stress_key_t *)key_in;

    if (!src || src->magic != KEY_MAGIC_LIVE) {
        atomic_fetch_add_explicit(&ctx->stats.invariant_violations, 1, memory_order_relaxed);
        *key_out = NULL;
        return;
    }

    stress_key_t *copy = (stress_key_t *)malloc(sizeof(stress_key_t));
    if (!copy) {
        *key_out = NULL;
        return;
    }

    memcpy(copy, src, sizeof(stress_key_t));
    *key_out = copy;
    atomic_fetch_add_explicit(&ctx->stats.keys_retained, 1, memory_order_relaxed);
}

static void stress_key_release_cb(void *key, void *user_data) {
    stress_context_t *ctx = (stress_context_t *)user_data;
    stress_key_t *k = (stress_key_t *)key;

    if (!k) {
        return;
    }
    if (k->magic != KEY_MAGIC_LIVE) {
        atomic_fetch_add_explicit(&ctx->stats.invariant_violations, 1, memory_order_relaxed);
        fprintf(stderr,
                "[FATAL INVARIANT] Double key release or corrupt key magic: 0x%016" PRIx64 "\n",
                k->magic);
        return;
    }

    k->magic = KEY_MAGIC_FREED;
    free(k);
    atomic_fetch_add_explicit(&ctx->stats.keys_released, 1, memory_order_relaxed);
}

static void stress_value_release_cb(void *value, void *user_data) {
    stress_context_t *ctx = (stress_context_t *)user_data;
    stress_value_t *val = (stress_value_t *)value;

    if (!val) {
        return;
    }

    /* Verify canary header to detect double-release or memory corruption */
    if (val->magic_header != VALUE_MAGIC_LIVE) {
        atomic_fetch_add_explicit(&ctx->stats.invariant_violations, 1, memory_order_relaxed);
        fprintf(stderr,
                "[FATAL INVARIANT] value_release_cb on non-live value %p (magic=0x%016" PRIx64 ")\n",
                value, val->magic_header);
        return;
    }

    /* Verify no reader thread currently holds an active reference */
    int32_t readers = atomic_load_explicit(&val->active_readers, memory_order_acquire);
    if (readers != 0) {
        atomic_fetch_add_explicit(&ctx->stats.invariant_violations, 1, memory_order_relaxed);
        fprintf(stderr,
                "[FATAL INVARIANT] Premature eviction! Value for key=%" PRIu64
                " freed while active_readers=%d\n",
                val->key_id, readers);
    }

    val->magic_header = VALUE_MAGIC_FREED;

    if (val->vm_page_addr != 0 && val->vm_page_size > 0) {
        mach_vm_deallocate(mach_task_self(), val->vm_page_addr, val->vm_page_size);
        val->vm_page_addr = 0;
    }

    free(val);
    atomic_fetch_add_explicit(&ctx->stats.values_released, 1, memory_order_relaxed);
}

/*
 * Invoked by libcache when an entry's internal reference count drops to 0
 * (i.e. all external callers have invoked cache_release_value, leaving the item
 * cached but unretained).
 */
static void stress_value_make_purgeable_cb(void *value, void *user_data) {
    stress_context_t *ctx = (stress_context_t *)user_data;
    stress_value_t *val = (stress_value_t *)value;

    if (!val || val->magic_header != VALUE_MAGIC_LIVE) {
        atomic_fetch_add_explicit(&ctx->stats.invariant_violations, 1, memory_order_relaxed);
        return;
    }

    atomic_store_explicit(&val->is_purgeable, true, memory_order_release);
    atomic_fetch_add_explicit(&ctx->stats.make_purgeable_calls, 1, memory_order_relaxed);

    if (ctx->cfg.use_mach_purgeable && val->vm_page_addr != 0) {
        int state = VM_PURGABLE_VOLATILE | VM_VOLATILE_GROUP_DEFAULT;
        (void)vm_purgable_control(
            mach_task_self(), (vm_address_t)val->vm_page_addr, VM_PURGABLE_SET_STATE, &state);
    }
}

/*
 * Invoked by libcache when a caller requests an unretained cached entry via
 * cache_get_and_retain. Returns true if the value is still valid and now
 * non-purgeable, or false if the kernel purged its backing page while volatile.
 */
static bool stress_value_make_nonpurgeable_cb(void *value, void *user_data) {
    stress_context_t *ctx = (stress_context_t *)user_data;
    stress_value_t *val = (stress_value_t *)value;

    if (!val || val->magic_header != VALUE_MAGIC_LIVE) {
        atomic_fetch_add_explicit(&ctx->stats.invariant_violations, 1, memory_order_relaxed);
        return false;
    }

    atomic_store_explicit(&val->is_purgeable, false, memory_order_release);
    atomic_fetch_add_explicit(&ctx->stats.make_nonpurgeable_calls, 1, memory_order_relaxed);

    if (ctx->cfg.use_mach_purgeable && val->vm_page_addr != 0) {
        int state = VM_PURGABLE_NONVOLATILE;
        kern_return_t kr = vm_purgable_control(
            mach_task_self(), (vm_address_t)val->vm_page_addr, VM_PURGABLE_SET_STATE, &state);
        if (kr != KERN_SUCCESS || (state & VM_PURGABLE_EMPTY)) {
            atomic_store_explicit(&val->was_purged_by_kernel, true, memory_order_release);
            atomic_fetch_add_explicit(
                &ctx->stats.volatile_emptied_events, 1, memory_order_relaxed);
            return false;
        }
    }

    /* Under critical memory pressure, probabilistically simulate kernel page reclaim */
    int pressure = atomic_load_explicit(&ctx->stats.current_pressure_level, memory_order_relaxed);
    if (pressure == PRESSURE_LEVEL_CRITICAL && ((val->generation & 0x7) == 0)) {
        atomic_fetch_add_explicit(&ctx->stats.volatile_emptied_events, 1, memory_order_relaxed);
        return false;
    }

    return true;
}

/* ========================================================================== *
 * Value Allocation & Integrity Verification                                  *
 * ========================================================================== */

static stress_value_t *allocate_stress_value(stress_context_t *ctx,
                                             uint64_t key_id,
                                             uint64_t generation,
                                             size_t payload_size) {
    stress_value_t *val =
        (stress_value_t *)malloc(sizeof(stress_value_t) + payload_size);
    if (!val) {
        return NULL;
    }

    val->magic_header = VALUE_MAGIC_LIVE;
    val->key_id = key_id;
    val->generation = generation;
    atomic_init(&val->active_readers, 0);
    atomic_init(&val->is_purgeable, false);
    atomic_init(&val->was_purged_by_kernel, false);
    val->cost = sizeof(stress_value_t) + payload_size;
    val->payload_size = payload_size;
    val->vm_page_addr = 0;
    val->vm_page_size = 0;

    if (ctx->cfg.use_mach_purgeable) {
        mach_vm_address_t addr = 0;
        mach_vm_size_t pg_size = vm_page_size;
        kern_return_t kr = mach_vm_allocate(
            mach_task_self(), &addr, pg_size, VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE);
        if (kr == KERN_SUCCESS) {
            val->vm_page_addr = addr;
            val->vm_page_size = pg_size;
            val->cost += (size_t)pg_size;
        }
    }

    /* Fill deterministic pattern derived from (key_id, generation) */
    uint8_t seed = (uint8_t)((key_id ^ (generation << 3)) & 0xFF);
    for (size_t i = 0; i < payload_size; i++) {
        val->payload[i] = (uint8_t)(seed + (i * 31));
    }

    val->checksum = compute_payload_checksum(
        key_id, generation, val->payload, payload_size);

    atomic_fetch_add_explicit(&ctx->stats.values_allocated, 1, memory_order_relaxed);
    return val;
}

static void verify_value_integrity(stress_context_t *ctx,
                                   uint64_t expected_key_id,
                                   stress_value_t *val) {
    if (!val) {
        atomic_fetch_add_explicit(&ctx->stats.invariant_violations, 1, memory_order_relaxed);
        return;
    }

    /* Track active reader reference to detect premature eviction */
    atomic_fetch_add_explicit(&val->active_readers, 1, memory_order_acq_rel);

    if (val->magic_header != VALUE_MAGIC_LIVE) {
        atomic_fetch_add_explicit(&ctx->stats.invariant_violations, 1, memory_order_relaxed);
        fprintf(stderr,
                "[FATAL INVARIANT] Reader obtained freed/corrupt value %p (magic=0x%016" PRIx64 ")\n",
                (void *)val, val->magic_header);
    } else if (val->key_id != expected_key_id) {
        atomic_fetch_add_explicit(&ctx->stats.invariant_violations, 1, memory_order_relaxed);
        fprintf(stderr,
                "[FATAL INVARIANT] Key mismatch on retrieval! Expected %" PRIu64 ", got %" PRIu64 "\n",
                expected_key_id, val->key_id);
    } else {
        uint64_t actual_crc = compute_payload_checksum(
            val->key_id, val->generation, val->payload, val->payload_size);
        if (actual_crc != val->checksum) {
            atomic_fetch_add_explicit(&ctx->stats.invariant_violations, 1, memory_order_relaxed);
            fprintf(stderr,
                    "[FATAL INVARIANT] Payload checksum mismatch for key %" PRIu64
                    " (expected 0x%016" PRIx64 ", got 0x%016" PRIx64 ")\n",
                    expected_key_id, val->checksum, actual_crc);
        }
        atomic_fetch_add_explicit(&ctx->stats.checksum_verifications, 1, memory_order_relaxed);
    }

    atomic_fetch_sub_explicit(&val->active_readers, 1, memory_order_acq_rel);
}

/* ========================================================================== *
 * Partition Eviction & Integrity Probe Sweeps (Public <cache.h> APIs)        *
 * ========================================================================== */

static void run_partition_eviction_sweep(stress_context_t *ctx,
                                         uint64_t modulus,
                                         uint64_t remainder,
                                         uint64_t max_keys_to_scan) {
    if (modulus == 0) {
        modulus = 1;
    }
    uint64_t limit = ctx->cfg.key_space < max_keys_to_scan
                         ? ctx->cfg.key_space
                         : max_keys_to_scan;

    for (uint64_t k = remainder; k < limit; k += modulus) {
        stress_key_t sweep_key;
        sweep_key.magic = KEY_MAGIC_LIVE;
        sweep_key.key_id = k;
        snprintf(sweep_key.label, sizeof(sweep_key.label), "darwin.cache.k.%" PRIu64, k);

        if (cache_remove(ctx->cache, &sweep_key) == 0) {
            atomic_fetch_add_explicit(&ctx->stats.explicit_removes, 1, memory_order_relaxed);
        }
    }
    atomic_fetch_add_explicit(&ctx->stats.partition_sweeps, 1, memory_order_relaxed);
}

static void run_integrity_probe_sweep(stress_context_t *ctx,
                                      uint64_t start_key,
                                      uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t k = (start_key + i) % ctx->cfg.key_space;
        stress_key_t probe_key;
        probe_key.magic = KEY_MAGIC_LIVE;
        probe_key.key_id = k;
        snprintf(probe_key.label, sizeof(probe_key.label), "darwin.cache.k.%" PRIu64, k);

        void *val_out = NULL;
        if (cache_get_and_retain(ctx->cache, &probe_key, &val_out) == 0 && val_out != NULL) {
            atomic_fetch_add_explicit(&ctx->stats.gets_hit, 1, memory_order_relaxed);
            verify_value_integrity(ctx, k, (stress_value_t *)val_out);
            cache_release_value(ctx->cache, val_out);
        }
    }
    atomic_fetch_add_explicit(&ctx->stats.probe_sweeps, 1, memory_order_relaxed);
}

/* ========================================================================== *
 * Worker Thread Routine                                                      *
 * ========================================================================== */

static void *stress_worker_main(void *arg) {
    worker_arg_t *w = (worker_arg_t *)arg;
    stress_context_t *ctx = w->ctx;
    uint64_t rng = w->rng_state;
    uint64_t local_gen = ((uint64_t)w->thread_id << 32) | 1ULL;

    while (!atomic_load_explicit(&ctx->stop_flag, memory_order_relaxed) && !g_interrupted) {
        uint64_t key_id = select_key_id(&rng, ctx->cfg.key_space, ctx->cfg.use_zipfian);
        stress_key_t lookup_key;
        lookup_key.magic = KEY_MAGIC_LIVE;
        lookup_key.key_id = key_id;
        snprintf(lookup_key.label, sizeof(lookup_key.label), "darwin.cache.k.%" PRIu64, key_id);

        uint64_t op_roll = xorshift64s(&rng) % 100;

        if (op_roll < 55) {
            /* -------------------------------------------------------------- *
             * 55%: Concurrent Retrieval (cache_get_and_retain)               *
             * -------------------------------------------------------------- */
            void *val_out = NULL;
            uint64_t t0 = mach_absolute_time();
            int rc = cache_get_and_retain(ctx->cache, &lookup_key, &val_out);
            uint64_t elapsed_ns = mach_ticks_to_ns(mach_absolute_time() - t0, &ctx->timebase);

            atomic_fetch_add_explicit(&ctx->stats.total_get_latency_ns, elapsed_ns, memory_order_relaxed);
            update_max_u64(&ctx->stats.max_get_latency_ns, elapsed_ns);

            if (rc == 0 && val_out != NULL) {
                atomic_fetch_add_explicit(&ctx->stats.gets_hit, 1, memory_order_relaxed);
                stress_value_t *val = (stress_value_t *)val_out;

                verify_value_integrity(ctx, key_id, val);

                /* Release retained value reference back to libcache */
                cache_release_value(ctx->cache, val);
            } else {
                atomic_fetch_add_explicit(&ctx->stats.gets_miss, 1, memory_order_relaxed);
            }
        } else if (op_roll < 90) {
            /* -------------------------------------------------------------- *
             * 35%: Concurrent Insertion (cache_set_and_retain)               *
             * -------------------------------------------------------------- */
            size_t span = ctx->cfg.max_payload_bytes > ctx->cfg.min_payload_bytes
                              ? (ctx->cfg.max_payload_bytes - ctx->cfg.min_payload_bytes)
                              : 1;
            size_t payload_len = ctx->cfg.min_payload_bytes + (xorshift64s(&rng) % span);

            stress_value_t *new_val =
                allocate_stress_value(ctx, key_id, local_gen++, payload_len);
            if (!new_val) {
                continue;
            }

            /* Hold an active reader claim while we own the retained reference */
            atomic_fetch_add_explicit(&new_val->active_readers, 1, memory_order_acq_rel);

            uint64_t t0 = mach_absolute_time();
            int rc = cache_set_and_retain(ctx->cache, &lookup_key, new_val, new_val->cost);
            uint64_t elapsed_ns = mach_ticks_to_ns(mach_absolute_time() - t0, &ctx->timebase);
            atomic_fetch_add_explicit(&ctx->stats.total_set_latency_ns, elapsed_ns, memory_order_relaxed);

            if (rc == 0) {
                atomic_fetch_add_explicit(&ctx->stats.insertions_total, 1, memory_order_relaxed);
                verify_value_integrity(ctx, key_id, new_val);
                atomic_fetch_sub_explicit(&new_val->active_readers, 1, memory_order_acq_rel);
                cache_release_value(ctx->cache, new_val);
            } else {
                atomic_fetch_sub_explicit(&new_val->active_readers, 1, memory_order_acq_rel);
            }
        } else if (op_roll < 97) {
            /* -------------------------------------------------------------- *
             * 7%: Explicit Key Eviction (cache_remove)                       *
             * -------------------------------------------------------------- */
            if (cache_remove(ctx->cache, &lookup_key) == 0) {
                atomic_fetch_add_explicit(&ctx->stats.explicit_removes, 1, memory_order_relaxed);
            }
        } else if (op_roll < 99) {
            /* -------------------------------------------------------------- *
             * 2%: Partition Stride Eviction Sweep (cache_remove)             *
             * -------------------------------------------------------------- */
            uint64_t target_rem = xorshift64s(&rng) % 8;
            run_partition_eviction_sweep(ctx, 8, target_rem, 128);
        } else {
            /* -------------------------------------------------------------- *
             * 1%: Multi-Key Integrity Probe Sweep                            *
             * -------------------------------------------------------------- */
            run_integrity_probe_sweep(ctx, key_id, 16);
        }
    }

    return NULL;
}

/* ========================================================================== *
 * Darwin Memory Pressure Subsystem & Synthetic Wave Generator                *
 * ========================================================================== */

static void setup_darwin_memory_pressure_Monitor(stress_context_t *ctx) {
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    ctx->memory_pressure_source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_MEMORYPRESSURE,
        0,
        DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
        q);

    if (!ctx->memory_pressure_source) {
        return;
    }

    dispatch_source_set_event_handler(ctx->memory_pressure_source, ^{
        unsigned long flags = dispatch_source_get_data(ctx->memory_pressure_source);
        atomic_fetch_add_explicit(&ctx->stats.os_pressure_events, 1, memory_order_relaxed);

        if (flags & DISPATCH_MEMORYPRESSURE_CRITICAL) {
            atomic_store_explicit(&ctx->stats.current_pressure_level, PRESSURE_LEVEL_CRITICAL, memory_order_relaxed);
            if (ctx->cfg.verbose) {
                fprintf(stderr, "[OS MEMORY PRESSURE] DISPATCH_MEMORYPRESSURE_CRITICAL received\n");
            }
            cache_remove_all(ctx->cache);
            atomic_fetch_add_explicit(&ctx->stats.full_purges, 1, memory_order_relaxed);
        } else if (flags & DISPATCH_MEMORYPRESSURE_WARN) {
            atomic_store_explicit(&ctx->stats.current_pressure_level, PRESSURE_LEVEL_WARN, memory_order_relaxed);
            if (ctx->cfg.verbose) {
                fprintf(stderr, "[OS MEMORY PRESSURE] DISPATCH_MEMORYPRESSURE_WARN received\n");
            }
            run_partition_eviction_sweep(ctx, 2, 0, ctx->cfg.key_space);
        } else {
            atomic_store_explicit(&ctx->stats.current_pressure_level, PRESSURE_LEVEL_NORMAL, memory_order_relaxed);
        }
    });

    dispatch_resume(ctx->memory_pressure_source);
}

static void *memory_pressure_wave_main(void *arg) {
    stress_context_t *ctx = (stress_context_t *)arg;
    uint32_t cycle = 0;

    while (!atomic_load_explicit(&ctx->stop_flag, memory_order_relaxed) && !g_interrupted) {
        usleep(350 * 1000); /* 350ms per phase */
        if (atomic_load_explicit(&ctx->stop_flag, memory_order_relaxed) || g_interrupted) {
            break;
        }

        cycle++;
        pressure_level_t next_level = (pressure_level_t)(cycle % 3);
        atomic_store_explicit(&ctx->stats.current_pressure_level, next_level, memory_order_relaxed);
        atomic_fetch_add_explicit(&ctx->stats.synthetic_pressure_waves, 1, memory_order_relaxed);

        if (next_level == PRESSURE_LEVEL_WARN) {
            run_partition_eviction_sweep(ctx, 2, cycle & 1, 512);
        } else if (next_level == PRESSURE_LEVEL_CRITICAL) {
            mach_vm_address_t ballast_addr = 0;
            mach_vm_size_t ballast_bytes = (mach_vm_size_t)ctx->cfg.pressure_ballast_mb * 1024ULL * 1024ULL;
            if (ballast_bytes > 0 &&
                mach_vm_allocate(mach_task_self(), &ballast_addr, ballast_bytes, VM_FLAGS_ANYWHERE) == KERN_SUCCESS) {
                volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)ballast_addr;
                for (mach_vm_size_t off = 0; off < ballast_bytes; off += vm_page_size) {
                    p[off] = (uint8_t)(off & 0xFF);
                }
                cache_remove_all(ctx->cache);
                atomic_fetch_add_explicit(&ctx->stats.full_purges, 1, memory_order_relaxed);
                mach_vm_deallocate(mach_task_self(), ballast_addr, ballast_bytes);
            } else {
                cache_remove_all(ctx->cache);
                atomic_fetch_add_explicit(&ctx->stats.full_purges, 1, memory_order_relaxed);
            }
        }
    }

    atomic_store_explicit(&ctx->stats.current_pressure_level, PRESSURE_LEVEL_NORMAL, memory_order_relaxed);
    return NULL;
}

/* ========================================================================== *
 * CLI Argument Parsing, Usage Text & Post-Teardown Accounting                *
 * ========================================================================== */

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Darwin libcache Concurrent Stress Test & Memory Pressure Verifier\n");
    printf("Exercises Apple's <cache.h> and <cache_callbacks.h> APIs under concurrent\n");
    printf("insertions, retrievals, evictions, and kernel memory pressure events.\n\n");
    printf("Options:\n");
    printf("  -t, --threads <N>          Number of worker pthreads (default: 12)\n");
    printf("  -d, --duration <sec>       Stress test run duration in seconds (default: 10)\n");
    printf("  -k, --keys <N>             Distinct key space cardinality (default: 2048)\n");
    printf("  -n, --min-bytes <bytes>    Minimum value payload size in bytes (default: 256)\n");
    printf("  -x, --max-bytes <bytes>    Maximum value payload size in bytes (default: 4096)\n");
    printf("  -b, --ballast <MB>         Mach VM dirty pressure ballast in MB (default: 32)\n");
    printf("  -z, --zipf                 Use Zipfian hot-key distribution (default)\n");
    printf("  -u, --uniform              Use Uniform key distribution\n");
    printf("  -m, --mach-purgeable       Enable Mach VM_FLAGS_PURGABLE backing pages\n");
    printf("  -M, --no-mach-purgeable    Disable Mach VM_FLAGS_PURGABLE backing pages\n");
    printf("  -v, --verbose              Log real-time OS memory pressure events\n");
    printf("  -h, --help                 Display this usage information and exit\n\n");
    printf("Workload Mix:\n");
    printf("  55%%%% cache_get_and_retain() + FNV-1a checksum audit + cache_release_value()\n");
    printf("  35%%%% cache_set_and_retain() + canary initialization + cache_release_value()\n");
    printf("   7%%%% cache_remove() single-key eviction\n");
    printf("   2%%%% cache_remove() partition stride eviction sweep\n");
    printf("   1%%%% multi-key integrity probe sweep\n\n");
    printf("Examples:\n");
    printf("  %s --threads 16 --duration 15 --keys 4096 --zipf --mach-purgeable\n", prog);
    printf("  MallocStackLogging=1 leaks --atExit -- %s -t 12 -d 10 -m\n", prog);
}

int main(int argc, char **argv) {
    stress_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Defaults populated from Workbench configuration */
    ctx.cfg.num_threads         = 12;
    ctx.cfg.duration_sec        = 10;
    ctx.cfg.key_space           = 2048ULL;
    ctx.cfg.min_payload_bytes   = 256;
    ctx.cfg.max_payload_bytes   = 4096;
    ctx.cfg.pressure_ballast_mb = 32;
    ctx.cfg.use_zipfian         = true;
    ctx.cfg.use_mach_purgeable  = true;
    ctx.cfg.verbose             = false;

    static struct option long_opts[] = {
        {"threads",           required_argument, 0, 't'},
        {"duration",          required_argument, 0, 'd'},
        {"keys",              required_argument, 0, 'k'},
        {"min-bytes",         required_argument, 0, 'n'},
        {"max-bytes",         required_argument, 0, 'x'},
        {"ballast",           required_argument, 0, 'b'},
        {"zipf",              no_argument,       0, 'z'},
        {"uniform",           no_argument,       0, 'u'},
        {"mach-purgeable",    no_argument,       0, 'm'},
        {"no-mach-purgeable", no_argument,       0, 'M'},
        {"verbose",           no_argument,       0, 'v'},
        {"help",              no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:d:k:n:x:b:zumMvh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 't': ctx.cfg.num_threads = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'd': ctx.cfg.duration_sec = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'k': ctx.cfg.key_space = strtoull(optarg, NULL, 10); break;
            case 'n': ctx.cfg.min_payload_bytes = (size_t)strtoull(optarg, NULL, 10); break;
            case 'x': ctx.cfg.max_payload_bytes = (size_t)strtoull(optarg, NULL, 10); break;
            case 'b': ctx.cfg.pressure_ballast_mb = (size_t)strtoull(optarg, NULL, 10); break;
            case 'z': ctx.cfg.use_zipfian = true; break;
            case 'u': ctx.cfg.use_zipfian = false; break;
            case 'm': ctx.cfg.use_mach_purgeable = true; break;
            case 'M': ctx.cfg.use_mach_purgeable = false; break;
            case 'v': ctx.cfg.verbose = true; break;
            case 'h':
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "Error: Unexpected positional argument '%s'\n\n", argv[optind]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (ctx.cfg.num_threads < 1) ctx.cfg.num_threads = 1;
    if (ctx.cfg.key_space < 8) ctx.cfg.key_space = 8;
    if (ctx.cfg.min_payload_bytes < 16) ctx.cfg.min_payload_bytes = 16;
    if (ctx.cfg.max_payload_bytes < ctx.cfg.min_payload_bytes) {
        ctx.cfg.max_payload_bytes = ctx.cfg.min_payload_bytes;
    }

    signal(SIGINT, handle_sigint);
    mach_timebase_info(&ctx.timebase);

    cache_attributes_t attrs = {
        .version                    = CACHE_ATTRIBUTES_VERSION_2,
        .key_hash_cb                = stress_key_hash_cb,
        .key_is_equal_cb            = stress_key_is_equal_cb,
        .key_retain_cb              = stress_key_retain_cb,
        .key_release_cb             = stress_key_release_cb,
        .value_release_cb           = stress_value_release_cb,
        .value_make_purgeable_cb    = stress_value_make_purgeable_cb,
        .value_make_nonpurgeable_cb = stress_value_make_nonpurgeable_cb,
        .user_data                  = &ctx
    };

    int rc = cache_create("com.darwin.libcache.stress.suite", &attrs, &ctx.cache);
    if (rc != 0 || !ctx.cache) {
        fprintf(stderr, "Fatal: cache_create() failed with error code %d\n", rc);
        return EXIT_FAILURE;
    }

    printf("========================================================================\n");
    printf("Darwin libcache Concurrent Stress & Memory Pressure Verifier\n");
    printf("========================================================================\n");
    printf("Cache Identifier : com.darwin.libcache.stress.suite\n");
    printf("Worker Threads   : %u pthreads + 1 Memory Pressure Wave + GCD Listener\n",
           ctx.cfg.num_threads);
    printf("Key Distribution : %s (%" PRIu64 " distinct keys)\n",
           ctx.cfg.use_zipfian ? "Zipfian (Hot-Key Contention)" : "Uniform",
           ctx.cfg.key_space);
    printf("Payload Range    : %zu B .. %zu B (Mach VM Purgeable: %s)\n",
           ctx.cfg.min_payload_bytes,
           ctx.cfg.max_payload_bytes,
           ctx.cfg.use_mach_purgeable ? "ENABLED" : "DISABLED");
    printf("Duration         : %u seconds\n", ctx.cfg.duration_sec);
    printf("------------------------------------------------------------------------\n");

    setup_darwin_memory_pressure_Monitor(&ctx);

    pthread_t *workers = (pthread_t *)calloc(ctx.cfg.num_threads, sizeof(pthread_t));
    worker_arg_t *wargs = (worker_arg_t *)calloc(ctx.cfg.num_threads, sizeof(worker_arg_t));
    pthread_t pressure_tid;

    uint64_t start_ticks = mach_absolute_time();

    for (uint32_t i = 0; i < ctx.cfg.num_threads; i++) {
        wargs[i].ctx = &ctx;
        wargs[i].thread_id = i + 1;
        wargs[i].rng_state = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)(i + 1) * 0xBF58476D1CE4E5B9ULL);
        pthread_create(&workers[i], NULL, stress_worker_main, &wargs[i]);
    }

    pthread_create(&pressure_tid, NULL, memory_pressure_wave_main, &ctx);

    for (uint32_t elapsed = 1; elapsed <= ctx.cfg.duration_sec && !g_interrupted; elapsed++) {
        sleep(1);
        uint64_t hits = atomic_load_explicit(&ctx.stats.gets_hit, memory_order_relaxed);
        uint64_t misses = atomic_load_explicit(&ctx.stats.gets_miss, memory_order_relaxed);
        uint64_t sets = atomic_load_explicit(&ctx.stats.insertions_total, memory_order_relaxed);
        uint64_t v_alloc = atomic_load_explicit(&ctx.stats.values_allocated, memory_order_relaxed);
        uint64_t v_rel = atomic_load_explicit(&ctx.stats.values_released, memory_order_relaxed);
        int p_lvl = atomic_load_explicit(&ctx.stats.current_pressure_level, memory_order_relaxed);

        const char *p_label = (p_lvl == PRESSURE_LEVEL_CRITICAL)
                                  ? "CRITICAL"
                                  : (p_lvl == PRESSURE_LEVEL_WARN ? "WARN" : "NORMAL");

        printf("[%2us/%2us] ops=%9" PRIu64 " | hits=%8" PRIu64 " | miss=%8" PRIu64
               " | live_vals=%6" PRId64 " | pressure=%-8s\n",
               elapsed,
               ctx.cfg.duration_sec,
               hits + misses + sets,
               hits,
               misses,
               (int64_t)(v_alloc - v_rel),
               p_label);
    }

    atomic_store_explicit(&ctx.stop_flag, true, memory_order_release);

    pthread_join(pressure_tid, NULL);
    for (uint32_t i = 0; i < ctx.cfg.num_threads; i++) {
        pthread_join(workers[i], NULL);
    }

    if (ctx.memory_pressure_source) {
        dispatch_source_cancel(ctx.memory_pressure_source);
    }

    /* Final integrity probe sweep and complete teardown to verify 100% callback reclamation */
    run_integrity_probe_sweep(&ctx, 0, ctx.cfg.key_space < 256 ? ctx.cfg.key_space : 256);
    cache_remove_all(ctx.cache);

    int destroy_rc = cache_destroy(ctx.cache);
    if (destroy_rc != 0) {
        fprintf(stderr, "[WARN] cache_destroy returned %d\n", destroy_rc);
    }

    uint64_t total_ns = mach_ticks_to_ns(mach_absolute_time() - start_ticks, &ctx.timebase);
    double elapsed_sec = (double)total_ns / 1e9;

    uint64_t hits       = atomic_load(&ctx.stats.gets_hit);
    uint64_t misses     = atomic_load(&ctx.stats.gets_miss);
    uint64_t sets       = atomic_load(&ctx.stats.insertions_total);
    uint64_t removes    = atomic_load(&ctx.stats.explicit_removes);
    uint64_t sweeps     = atomic_load(&ctx.stats.partition_sweeps);
    uint64_t purges     = atomic_load(&ctx.stats.full_purges);
    uint64_t k_ret      = atomic_load(&ctx.stats.keys_retained);
    uint64_t k_rel      = atomic_load(&ctx.stats.keys_released);
    uint64_t v_alloc    = atomic_load(&ctx.stats.values_allocated);
    uint64_t v_rel      = atomic_load(&ctx.stats.values_released);
    uint64_t violations = atomic_load(&ctx.stats.invariant_violations);
    uint64_t crc_checks = atomic_load(&ctx.stats.checksum_verifications);

    uint64_t total_gets = hits + misses;
    uint64_t avg_get_ns = total_gets > 0 ? (atomic_load(&ctx.stats.total_get_latency_ns) / total_gets) : 0;
    uint64_t avg_set_ns = sets > 0 ? (atomic_load(&ctx.stats.total_set_latency_ns) / sets) : 0;

    printf("------------------------------------------------------------------------\n");
    printf("Post-Teardown Audit & Accounting Summary (%.2f sec)\n", elapsed_sec);
    printf("------------------------------------------------------------------------\n");
    printf("Total Operations         : %" PRIu64 " (%.0f ops/sec)\n",
           total_gets + sets + removes,
           (double)(total_gets + sets + removes) / (elapsed_sec > 0 ? elapsed_sec : 1.0));
    printf("Cache Get Hits / Misses  : %" PRIu64 " / %" PRIu64 " (%.1f%%%% hit ratio)\n",
           hits,
           misses,
           total_gets > 0 ? (100.0 * (double)hits / (double)total_gets) : 0.0);
    printf("Cache Set Insertions     : %" PRIu64 " (avg %" PRIu64 " ns/op)\n", sets, avg_set_ns);
    printf("Cache Get Latency        : avg %" PRIu64 " ns/op | max %" PRIu64 " ns\n",
           avg_get_ns,
           atomic_load(&ctx.stats.max_get_latency_ns));
    printf("Evictions (Key/Swp/All)  : %" PRIu64 " / %" PRIu64 " / %" PRIu64 "\n",
           removes, sweeps, purges);
    printf("Purgeable Transitions    : %" PRIu64 " volatile | %" PRIu64 " non-volatile | %" PRIu64 " kernel-emptied\n",
           atomic_load(&ctx.stats.make_purgeable_calls),
           atomic_load(&ctx.stats.make_nonpurgeable_calls),
           atomic_load(&ctx.stats.volatile_emptied_events));
    printf("Keys Retained / Released : %" PRIu64 " / %" PRIu64 " (delta: %" PRId64 ")\n",
           k_ret, k_rel, (int64_t)(k_ret - k_rel));
    printf("Vals Allocated / Freed   : %" PRIu64 " / %" PRIu64 " (delta: %" PRId64 ")\n",
           v_alloc, v_rel, (int64_t)(v_alloc - v_rel));
    printf("FNV-1a Payload Checks    : %" PRIu64 " verified\n", crc_checks);
    printf("Invariant Violations     : %" PRIu64 "\n", violations);
    printf("========================================================================\n");

    free(workers);
    free(wargs);

    bool leaked = (k_ret != k_rel) || (v_alloc != v_rel);
    if (violations > 0 || leaked) {
        fprintf(stderr, "RESULT: FAILED (violations=%" PRIu64 ", key_delta=%" PRId64 ", val_delta=%" PRId64 ")\n",
                violations, (int64_t)(k_ret - k_rel), (int64_t)(v_alloc - v_rel));
        return EXIT_FAILURE;
    }

    printf("RESULT: PASSED (All libcache refcount, canary, and purgeable invariants held)\n");
    return EXIT_SUCCESS;
}
