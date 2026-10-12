/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Brian McKenzie. All rights reserved.
 */

#include <stddef.h>

#include "cache.h"
#include "cache_internal.h"

/**
 * @brief Unlinks an entry from the doubly linked LRU queue.
 * @details Caller must hold cache->lru_lock.
 * @param cache A valid cache instance.
 * @param entry Entry node to detach from the LRU list.
 */
void
detach_lru(cache_t *cache, cache_entry_t *entry)
{
    if (entry->lru_prev != NULL)
        entry->lru_prev->lru_next = entry->lru_next;
    else
        cache->head = entry->lru_next;

    if (entry->lru_next != NULL)
        entry->lru_next->lru_prev = entry->lru_prev;
    else
        cache->tail = entry->lru_prev;
}

/**
 * @brief Inserts an entry at the head (most-recently-used end) of the doubly
 *        linked LRU queue.
 * @details Caller must hold cache->lru_lock.
 * @param cache A valid cache instance.
 * @param entry Entry node to insert at the head of the LRU list.
 */
void
attach_lru_head(cache_t *cache, cache_entry_t *entry)
{
    entry->lru_next = cache->head;
    entry->lru_prev = NULL;
    if (cache->head != NULL)
        cache->head->lru_prev = entry;
    cache->head = entry;
    if (cache->tail == NULL)
        cache->tail = entry;
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
    size_t slot = cache_hash_pointer(entry->value) & cache->val_bucket_mask;
    entry->val_next = cache->val_buckets[slot];
    cache->val_buckets[slot] = entry;
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
    size_t slot = cache_hash_pointer(entry->value) & cache->val_bucket_mask;
    cache_entry_t *curr = cache->val_buckets[slot];
    cache_entry_t *prev = NULL;

    while (curr != NULL) {
        if (curr == entry) {
            if (prev != NULL)
                prev->val_next = curr->val_next;
            else
                cache->val_buckets[slot] = curr->val_next;
            curr->val_next = NULL;
            return;
        }
        prev = curr;
        curr = curr->val_next;
    }
}
