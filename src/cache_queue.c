/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#include <stddef.h>

#include "cache.h"
#include "cache_internal.h"

/**
 * @brief Unlinks an entry from whichever SLRU segment queue (probationary or
 *        protected) it currently resides in.
 * @details Caller must hold cache->lru_lock.
 * @param cache A valid cache instance.
 * @param entry Entry node to detach from the SLRU segment queue.
 */
void
detach_lru(cache_t *cache, cache_entry_t *entry)
{
    if (cache == NULL || entry == NULL)
        return;

    if (entry->is_protected) {
        if (entry->lru_prev != NULL)
            entry->lru_prev->lru_next = entry->lru_next;
        else
            cache->prot_head = entry->lru_next;

        if (entry->lru_next != NULL)
            entry->lru_next->lru_prev = entry->lru_prev;
        else
            cache->prot_tail = entry->lru_prev;

        if (cache->prot_count > 0)
            cache->prot_count--;
    } else {
        if (entry->lru_prev != NULL)
            entry->lru_prev->lru_next = entry->lru_next;
        else
            cache->prob_head = entry->lru_next;

        if (entry->lru_next != NULL)
            entry->lru_next->lru_prev = entry->lru_prev;
        else
            cache->prob_tail = entry->lru_prev;

        if (cache->prob_count > 0)
            cache->prob_count--;
    }

    entry->lru_next = NULL;
    entry->lru_prev = NULL;

    /* Maintain overall head and tail aliases */
    cache->head = cache->prot_head ? cache->prot_head : cache->prob_head;
    cache->tail = cache->prob_tail ? cache->prob_tail : cache->prot_tail;
}

/**
 * @brief Inserts an entry at the head (MRU end) of the probationary segment.
 * @details Caller must hold cache->lru_lock.
 * @param cache A valid cache instance.
 * @param entry Entry node to insert into probationary segment.
 */
void
attach_probation_head(cache_t *cache, cache_entry_t *entry)
{
    if (cache == NULL || entry == NULL)
        return;

    entry->is_protected = false;
    entry->lru_next = cache->prob_head;
    entry->lru_prev = NULL;
    if (cache->prob_head != NULL)
        cache->prob_head->lru_prev = entry;
    cache->prob_head = entry;
    if (cache->prob_tail == NULL)
        cache->prob_tail = entry;
    cache->prob_count++;

    /* Maintain overall head and tail aliases */
    cache->head = cache->prot_head ? cache->prot_head : cache->prob_head;
    cache->tail = cache->prob_tail ? cache->prob_tail : cache->prot_tail;
}

/**
 * @brief Inserts an entry at the head (MRU end) of the protected segment.
 * @details Caller must hold cache->lru_lock.
 * @param cache A valid cache instance.
 * @param entry Entry node to insert into protected segment.
 */
void
attach_protected_head(cache_t *cache, cache_entry_t *entry)
{
    if (cache == NULL || entry == NULL)
        return;

    entry->is_protected = true;
    entry->lru_next = cache->prot_head;
    entry->lru_prev = NULL;
    if (cache->prot_head != NULL)
        cache->prot_head->lru_prev = entry;
    cache->prot_head = entry;
    if (cache->prot_tail == NULL)
        cache->prot_tail = entry;
    cache->prot_count++;

    /* Maintain overall head and tail aliases */
    cache->head = cache->prot_head ? cache->prot_head : cache->prob_head;
    cache->tail = cache->prob_tail ? cache->prob_tail : cache->prot_tail;
}

/**
 * @brief Compatibility wrapper inserting at the probationary head.
 * @details Caller must hold cache->lru_lock.
 * @param cache A valid cache instance.
 * @param entry Entry node to insert.
 */
void
attach_lru_head(cache_t *cache, cache_entry_t *entry)
{
    attach_probation_head(cache, entry);
}

/**
 * @brief Promotes an accessed entry according to Segmented LRU (SLRU) rules.
 * @details If the entry is currently in the probationary segment, it is promoted
 *          to the protected segment MRU head. If it is already in the protected
 *          segment, it is moved to the MRU head of the protected segment. If
 *          the protected segment exceeds its capacity ceiling, its LRU tail entry
 *          is demoted back to the MRU head of the probationary segment.
 *          Caller must hold cache->lru_lock.
 * @param cache A valid cache instance.
 * @param entry Entry node that was accessed on a cache hit.
 */
void
touch_entry(cache_t *cache, cache_entry_t *entry)
{
    size_t count_limit, prot_limit;
    cache_entry_t *demote_node;

    if (cache == NULL || entry == NULL)
        return;

    count_limit = atomic_load_explicit(&cache->configured_count_hint,
        memory_order_relaxed);

    if (entry->is_protected) {
        /* Already in protected segment: refresh to MRU head */
        if (cache->prot_head != entry) {
            detach_lru(cache, entry);
            attach_protected_head(cache, entry);
        }
    } else {
        /* In probationary segment: promote on second access to protected segment */
        detach_lru(cache, entry);
        attach_protected_head(cache, entry);
    }

    /* Enforce protected segment capacity if a count limit is configured */
    if (count_limit > 0) {
        prot_limit = (count_limit * 4) / 5;
        if (prot_limit == 0 && count_limit > 0)
            prot_limit = 1;
        if (prot_limit >= count_limit)
            prot_limit = count_limit - 1;

        /* Demote excess entries from protected tail to probationary head */
        while (cache->prot_count > prot_limit && cache->prot_tail != NULL) {
            demote_node = cache->prot_tail;
            detach_lru(cache, demote_node);
            attach_probation_head(cache, demote_node);
        }
    }
}

/**
 * @brief Inserts an active entry into the value-pointer hash index.
 * @details Caller must hold cache->lru_lock.
 * @param cache A valid cache instance.
 * @param entry Entry node to index by its value pointer.
 */
void
attach_val_bucket(cache_t *cache, cache_entry_t *entry)
{
    size_t slot = cache_hash_pointer(entry->value) & cache->bucket_mask;
    entry->val_next = cache->buckets[slot].val_head;
    cache->buckets[slot].val_head = entry;
}

/**
 * @brief Removes an entry from the value-pointer hash index.
 * @details Caller must hold cache->lru_lock.
 * @param cache A valid cache instance.
 * @param entry Entry node to unlink from the value-pointer hash index.
 */
void
detach_val_bucket(cache_t *cache, cache_entry_t *entry)
{
    size_t slot = cache_hash_pointer(entry->value) & cache->bucket_mask;
    cache_entry_t *curr = cache->buckets[slot].val_head;
    cache_entry_t *prev = NULL;

    while (curr != NULL) {
        if (curr == entry) {
            if (prev != NULL)
                prev->val_next = curr->val_next;
            else
                cache->buckets[slot].val_head = curr->val_next;
            curr->val_next = NULL;
            return;
        }
        prev = curr;
        curr = curr->val_next;
    }
}
