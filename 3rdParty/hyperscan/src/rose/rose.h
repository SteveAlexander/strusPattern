/*
 * Copyright (c) 2015, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef ROSE_H
#define ROSE_H

#include "rose_types.h"
#include "rose_internal.h"
#include "runtime.h"
#include "scratch.h"
#include "ue2common.h"
#include "util/multibit.h"

// Initialise state space for engine use.
void roseInitState(const struct RoseEngine *t, u8 *state);

void roseBlockEodExec(const struct RoseEngine *t, u64a offset,
                      struct hs_scratch *scratch);
void roseBlockExec_i(const struct RoseEngine *t, struct hs_scratch *scratch,
                     RoseCallback callback, RoseCallbackSom som_callback,
                     void *context);

/* assumes core_info in scratch has been init to point to data */
static really_inline
void roseBlockExec(const struct RoseEngine *t, struct hs_scratch *scratch,
                   RoseCallback callback, RoseCallbackSom som_callback,
                   void *context) {
    assert(t);
    assert(scratch);
    assert(scratch->core_info.buf);

    // If this block is shorter than our minimum width, then no pattern in this
    // RoseEngine could match.
    /* minWidth checks should have already been performed by the caller */
    const size_t length = scratch->core_info.len;
    assert(length >= t->minWidth);

    // Similarly, we may have a maximum width (for engines constructed entirely
    // of bi-anchored patterns).
    /* This check is now handled by the interpreter */
    assert(t->maxBiAnchoredWidth == ROSE_BOUND_INF
           || length <= t->maxBiAnchoredWidth);

    roseBlockExec_i(t, scratch, callback, som_callback, context);

    if (!t->requiresEodCheck) {
        return;
    }

    if (can_stop_matching(scratch)) {
        DEBUG_PRINTF("bailing, already halted\n");
        return;
    }

    struct mmbit_sparse_state *s = scratch->sparse_iter_state;
    const u32 numStates = t->rolesWithStateCount;
    u8 *state = (u8 *)scratch->core_info.state;
    void *role_state = getRoleState(state);
    u32 idx = 0;
    const struct mmbit_sparse_iter *it
        = (const void *)((const u8 *)t + t->eodIterOffset);

    if (!t->ematcherOffset && !t->hasEodEventLiteral
        && !mmbit_any(getActiveLeafArray(t, state), t->activeArrayCount)
        && (!t->eodIterOffset
            || mmbit_sparse_iter_begin(role_state, numStates, &idx, it, s)
            == MMB_INVALID)) {
        return;
    }

    roseBlockEodExec(t, length, scratch);
}

/* assumes core_info in scratch has been init to point to data */
void roseStreamExec(const struct RoseEngine *t, u8 *state,
                    struct hs_scratch *scratch, RoseCallback callback,
                    RoseCallbackSom som_callback, void *context);

void roseEodExec(const struct RoseEngine *t, u8 *state, u64a offset,
                 struct hs_scratch *scratch, RoseCallback callback,
                 RoseCallbackSom som_callback, void *context);

#define ROSE_CONTINUE_MATCHING_NO_EXHAUST 2

#endif // ROSE_H
