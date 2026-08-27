/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * \file
 * Fence / per-op identity state for the NVMe-KV host shim's in-flight op slot.
 *
 * This is deliberately SPDK-free (no lib/nvme types) so the three security-
 * relevant decisions can be reasoned about and unit-tested without a live NVMe
 * target:
 *   1. discard a stale "orphan" completion whose op has already been abandoned,
 *   2. latch the shim POISONED after a timeout / transport failure so no later
 *      op is submitted while an orphaned hardware tracker may still be live, and
 *   3. QUARANTINE a staging buffer whose DMA tracker may still be live, freeing
 *      it only after a fencing teardown proves the tracker dead.
 *
 * Composition note: the shim embeds ONE fence, which is device-level state, and
 * one tag PER IN-FLIGHT OP. The tag carries that op's completion slot, so
 * concurrent ops never share one. Abandoning an op (timeout, transport failure)
 * marks its tag rather than freeing it, because the hardware tracker may still
 * be live: a late orphan then writes into memory that is still allocated and
 * that nobody reads. The tag and its staging buffer are released together at
 * the fencing teardown.
 */

#ifndef SPDK_FENCE_H
#define SPDK_FENCE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/** A quarantined staging buffer awaiting release after the fencing teardown. */
struct spdk_quarantine_node {
    void *buf;
    struct spdk_quarantine_node *next;
};

/**
 * Fence state for the shim's single in-flight op slot. Zero-initialization
 * (e.g. calloc) yields the clean, un-poisoned, empty-quarantine state, which is
 * what spdk_fence_init() also produces.
 */
struct spdk_fence {
    /*
     * Latched true once a timeout / transport failure left a possibly-live DMA
     * tracker. While set, begin() refuses every new op so no later op can be
     * corrupted by the orphan or DMA into a freed buffer; cleared only by
     * drain() after a fencing teardown.
     */
    bool poisoned;
    /* Staging buffers from poisoned ops, released only once drain() runs after
     * the fencing teardown proves the tracker dead. */
    struct spdk_quarantine_node *quarantine;
};

/**
 * Per-op state and completion callback argument. One per in-flight op: the
 * completion callback writes this op's status here, and the submitter reads it
 * back once done. Zero-initialization is the clean state.
 */
struct spdk_op_tag {
    struct spdk_fence *fence;
    /* Completion, written by the callback and read after the poll. */
    volatile bool op_done;
    volatile uint8_t op_sct;
    volatile uint8_t op_sc;
    volatile uint32_t op_cdw0;
    /*
     * Set once the submitter has given up on this op (timeout, transport
     * failure). A completion arriving afterwards is discarded instead of being
     * recorded, and the tag must stay allocated until the fencing teardown
     * proves the tracker dead.
     */
    bool abandoned;
};

/** Initialise a fence to the clean, un-poisoned, empty-quarantine state. */
void
spdk_fence_init(struct spdk_fence *f);

/**
 * Begin a new op. If the fence is poisoned, refuse (return false, change
 * nothing). Otherwise clear \c tag's completion slot and return true. Hand
 * \c tag to the submit call as its completion cb_arg.
 */
bool
spdk_fence_begin(struct spdk_fence *f, struct spdk_op_tag *tag);

/**
 * Record a completion for the op identified by \c tag. If that op was abandoned
 * -- a stale orphan from an op that already timed out -- the completion is
 * DISCARDED (return false). Otherwise the status is captured, op_done is
 * latched, and it returns true.
 */
bool
spdk_fence_complete(struct spdk_op_tag *tag, uint8_t sct, uint8_t sc, uint32_t cdw0);

/** True once this op's completion has been recorded. */
bool
spdk_fence_done(const struct spdk_op_tag *tag);

/**
 * Give up on \c tag's op: a completion arriving later is discarded. The tag
 * itself must stay allocated until spdk_fence_drain() runs, since the hardware
 * tracker may still write to it.
 */
void
spdk_fence_abandon(struct spdk_op_tag *tag);

/** Latch the fence poisoned after a timeout / transport failure. Idempotent. */
void
spdk_fence_poison(struct spdk_fence *f);

/** True while the fence is poisoned (refusing new ops until drain()). */
bool
spdk_fence_poisoned(const struct spdk_fence *f);

/**
 * Quarantine staging buffer \c buf whose op left a possibly-live DMA tracker,
 * deferring its free to drain(). \c buf == NULL is a no-op. Returns 0 on
 * success, or -ENOMEM if the quarantine node could not be allocated -- in which
 * case the caller MUST NOT free \c buf (leaking it is strictly safer than a
 * use-after-free into memory the recovered target may still DMA into).
 */
int
spdk_fence_quarantine(struct spdk_fence *f, void *buf);

/**
 * Release every quarantined buffer with \c free_buf (invoked exactly once per
 * buffer), empty the quarantine, and clear the poison latch. MUST be called
 * only after a fencing teardown (qpair free / ctrlr reset) has proven the
 * hardware trackers dead, so no completion can reference a freed buffer.
 */
void
spdk_fence_drain(struct spdk_fence *f, void (*free_buf)(void *));

#endif /* SPDK_FENCE_H */
