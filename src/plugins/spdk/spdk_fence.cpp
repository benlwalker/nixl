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

/*
 * Pure, SPDK-free implementation of the shim fence: stale-orphan discard,
 * poison latch, and staging-buffer quarantine. See spdk_fence.h.
 */

#include "spdk_fence.h"

#include <errno.h>
#include <stdlib.h>

void
spdk_fence_init(struct spdk_fence *f) {
    f->poisoned = false;
    f->quarantine = NULL;
}

bool
spdk_fence_begin(struct spdk_fence *f, struct spdk_op_tag *tag) {
    if (f->poisoned) {
        return false;
    }
    tag->fence = f;
    tag->op_done = false;
    tag->op_sct = 0;
    tag->op_sc = 0;
    tag->op_cdw0 = 0;
    tag->abandoned = false;
    return true;
}

bool
spdk_fence_complete(struct spdk_op_tag *tag, uint8_t sct, uint8_t sc, uint32_t cdw0) {
    if (tag->abandoned) {
        /* Stale orphan from an op the submitter already gave up on. Discard
         * it; the tag stays allocated until drain() so this write is safe. */
        return false;
    }
    tag->op_sct = sct;
    tag->op_sc = sc;
    tag->op_cdw0 = cdw0;
    tag->op_done = true;
    return true;
}

bool
spdk_fence_done(const struct spdk_op_tag *tag) {
    return tag->op_done;
}

void
spdk_fence_abandon(struct spdk_op_tag *tag) {
    tag->abandoned = true;
}

void
spdk_fence_poison(struct spdk_fence *f) {
    f->poisoned = true;
}

bool
spdk_fence_poisoned(const struct spdk_fence *f) {
    return f->poisoned;
}

int
spdk_fence_quarantine(struct spdk_fence *f, void *buf) {
    struct spdk_quarantine_node *n;

    if (buf == NULL) {
        return 0;
    }
    n = static_cast<struct spdk_quarantine_node *>(calloc(1, sizeof(*n)));
    if (n == NULL) {
        return -ENOMEM;
    }
    n->buf = buf;
    n->next = f->quarantine;
    f->quarantine = n;
    return 0;
}

void
spdk_fence_drain(struct spdk_fence *f, void (*free_buf)(void *)) {
    struct spdk_quarantine_node *n = f->quarantine;

    while (n != NULL) {
        struct spdk_quarantine_node *next = n->next;

        if (free_buf != NULL) {
            free_buf(n->buf);
        }
        free(n);
        n = next;
    }
    f->quarantine = NULL;
    f->poisoned = false;
}
