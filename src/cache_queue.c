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
detach(cache_t *cache, cache_entry_t *entry)
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
touch(cache_t *cache, cache_entry_t *entry)
{
    size_t count_limit, prot_limit;
    cache_entry_t *demote_node;

    if (cache == NULL || entry == NULL || entry->is_removed)
        return;

    if (entry->is_protected) {
        /* Fast path: entry is already at the protected MRU head */
        if (cache->prot_head == entry)
            return;

        /* Already protected: move to MRU head without changing prot_count */
        detach(cache, entry);
        attach_protected_head(cache, entry);
        return;
    }

    /* In probationary segment: promote to protected segment */
    detach(cache, entry);
    attach_protected_head(cache, entry);

    /* Enforce protected segment capacity only upon probationary promotion */
    count_limit = cache_get_effective_count_limit(cache);
    if (count_limit > 0) {
        prot_limit = (count_limit * 4) / 5;
        if (prot_limit == 0)
            prot_limit = 1;
        else if (prot_limit >= count_limit)
            prot_limit = count_limit - 1;

        /* Demote excess entries from protected tail to probationary head */
        while (cache->prot_count > prot_limit && cache->prot_tail != NULL) {
            demote_node = cache->prot_tail;
            detach(cache, demote_node);
            attach_probation_head(cache, demote_node);
        }
    }
}

/**
 * @brief Unlinks an entry from its key hash bucket collision chain.
 * @details Caller must hold the stripe lock protecting entry->key_hash.
 * @param cache A valid cache instance.
 * @param entry Entry node to unlink from the key hash bucket.
 */
void
detach_key_bucket(cache_t *cache, cache_entry_t *entry)
{
    cache_entry_t *curr, *prev;

    if (cache == NULL || entry == NULL)
        return;

    curr = GET_KEY_BUCKET(cache, entry->key_hash);
    prev = NULL;
    while (curr != NULL) {
        if (curr == entry) {
            if (prev != NULL)
                prev->next = curr->next;
            else
                GET_KEY_BUCKET(cache, entry->key_hash) = curr->next;
            curr->next = NULL;
            break;
        }
        prev = curr;
        curr = curr->next;
    }
}

/**
 * @brief Inserts an active entry into the value-pointer hash index.
 * @details Synchronized via the value stripe lock covering entry->value.
 * @param cache A valid cache instance.
 * @param entry Entry node to index by its value pointer.
 */
void
attach_val_bucket(cache_t *cache, cache_entry_t *entry)
{
    size_t val_hash;
    size_t slot;
    os_unfair_lock_t val_lock;

    if (cache == NULL || entry == NULL)
        return;

    val_hash = cache_hash_pointer(entry->value);
    slot = val_hash & cache->bucket_mask;
    val_lock = GET_VAL_STRIPE_LOCK(cache, val_hash);

    os_unfair_lock_lock(val_lock);
    entry->val_next = cache->buckets[slot].val_head;
    cache->buckets[slot].val_head = entry;
    os_unfair_lock_unlock(val_lock);
}

/**
 * @brief Removes an entry from the value-pointer hash index.
 * @details Synchronized via the value stripe lock covering entry->value.
 * @param cache A valid cache instance.
 * @param entry Entry node to unlink from the value-pointer hash index.
 */
void
detach_val_bucket(cache_t *cache, cache_entry_t *entry)
{
    size_t val_hash;
    size_t slot;
    os_unfair_lock_t val_lock;
    cache_entry_t *curr, *prev;

    if (cache == NULL || entry == NULL)
        return;

    val_hash = cache_hash_pointer(entry->value);
    slot = val_hash & cache->bucket_mask;
    val_lock = GET_VAL_STRIPE_LOCK(cache, val_hash);

    os_unfair_lock_lock(val_lock);
    curr = cache->buckets[slot].val_head;
    prev = NULL;

    while (curr != NULL) {
        if (curr == entry) {
            if (prev != NULL)
                prev->val_next = curr->val_next;
            else
                cache->buckets[slot].val_head = curr->val_next;
            curr->val_next = NULL;
            break;
        }
        prev = curr;
        curr = curr->val_next;
    }
    os_unfair_lock_unlock(val_lock);
}
