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

/** \file
 * \brief Runtime functions shared between various Rose runtime code.
 */

#ifndef ROSE_RUNTIME_H
#define ROSE_RUNTIME_H

#include "scratch.h"
#include "rose_internal.h"
#include "util/exhaust.h" // for isExhausted
#include "util/internal_report.h"
#include "util/partial_store.h"

/*
 * ROSE STATE LAYOUT:
 *   state multibit
 *   runtime state structure
 *   full history table
 *   last history table
 *   short history table
 *   short queues (two multibits)
 *   last queues (two multibits)
 *   active array
 *   delay rb dirty
 *   nfa state
 */

#define rose_inline really_inline

/** \brief Fetch runtime state ptr. */
static really_inline
struct RoseRuntimeState *getRuntimeState(u8 *state) {
    struct RoseRuntimeState *rs = (struct RoseRuntimeState *)(state);
    assert(ISALIGNED_N(rs, 8));
    return rs;
}

static really_inline
const void *getByOffset(const struct RoseEngine *t, u32 offset) {
    assert(offset < t->size);
    return (const u8 *)t + offset;
}

static really_inline
void *getRoleState(u8 *state) {
    return state + sizeof(struct RoseRuntimeState);
}

/** \brief Fetch the active array for suffix nfas. */
static really_inline
u8 *getActiveLeafArray(const struct RoseEngine *t, u8 *state) {
    return state + t->stateOffsets.activeLeafArray;
}

/** \brief Fetch the active array for rose nfas. */
static really_inline
u8 *getActiveLeftArray(const struct RoseEngine *t, u8 *state) {
    return state + t->stateOffsets.activeLeftArray;
}

static really_inline
const u32 *getAnchoredInverseMap(const struct RoseEngine *t) {
    return (const u32 *)(((const u8 *)t) + t->anchoredReportInverseMapOffset);
}

static really_inline
const u32 *getAnchoredMap(const struct RoseEngine *t) {
    return (const u32 *)(((const u8 *)t) + t->anchoredReportMapOffset);
}

static really_inline
rose_group loadGroups(const struct RoseEngine *t, const u8 *state) {
    return partial_load_u64a(state + t->stateOffsets.groups,
                             t->stateOffsets.groups_size);

}

static really_inline
void storeGroups(const struct RoseEngine *t, u8 *state, rose_group groups) {
    partial_store_u64a(state + t->stateOffsets.groups, groups,
                       t->stateOffsets.groups_size);
}

static really_inline
u8 * getFloatingMatcherState(const struct RoseEngine *t, u8 *state) {
    return state + t->stateOffsets.floatingMatcherState;
}

static really_inline
u8 *getLeftfixLagTable(const struct RoseEngine *t, u8 *state) {
    return state + t->stateOffsets.leftfixLagTable;
}

static really_inline
const u8 *getLeftfixLagTableConst(const struct RoseEngine *t, const u8 *state) {
    return state + t->stateOffsets.leftfixLagTable;
}

static rose_inline
char roseSuffixInfoIsExhausted(const struct RoseEngine *t,
                               const struct NfaInfo *info,
                               const char *exhausted) {
    if (!info->ekeyListOffset) {
        return 0;
    }

    DEBUG_PRINTF("check exhaustion -> start at %u\n", info->ekeyListOffset);

    /* END_EXHAUST terminated list */
    const u32 *ekeys = (const u32 *)((const char *)t + info->ekeyListOffset);
    while (*ekeys != END_EXHAUST) {
        DEBUG_PRINTF("check %u\n", *ekeys);
        if (!isExhausted(exhausted, *ekeys)) {
            DEBUG_PRINTF("not exhausted -> alive\n");
            return 0;
        }
        ++ekeys;
    }

    DEBUG_PRINTF("all ekeys exhausted -> dead\n");
    return 1;
}

static really_inline
char roseSuffixIsExhausted(const struct RoseEngine *t, u32 qi,
                           const char *exhausted) {
    DEBUG_PRINTF("check queue %u\n", qi);
    const struct NfaInfo *info = getNfaInfoByQueue(t, qi);
    return roseSuffixInfoIsExhausted(t, info, exhausted);
}

static really_inline
u32 has_chained_nfas(const struct RoseEngine *t) {
    return t->outfixBeginQueue;
}

/** \brief Fetch \ref internal_report structure for this internal ID. */
static really_inline
const struct internal_report *getInternalReport(const struct RoseEngine *t,
                                                ReportID intId) {
    const struct internal_report *reports =
        (const struct internal_report *)((const u8 *)t + t->intReportOffset);
    assert(intId < t->intReportCount);
    return reports + intId;
}

static really_inline
const struct RoseRole *getRoleByOffset(const struct RoseEngine *t, u32 offset) {
        const struct RoseRole *tr = (const void *)((const char *)t + offset);

        assert((size_t)(tr - getRoleTable(t)) < t->roleCount);
        DEBUG_PRINTF("get root role %zu\n", tr - getRoleTable(t));
        return tr;
}

#define ANCHORED_MATCH_SENTINEL (~0U)

static really_inline
void updateLastMatchOffset(struct RoseContext *tctxt, u64a offset) {
    DEBUG_PRINTF("match @%llu, last match @%llu\n", offset,
                 tctxt->lastMatchOffset);

    assert(offset >= tctxt->minMatchOffset);
    assert(offset >= tctxt->lastMatchOffset);
    tctxt->lastMatchOffset = offset;
}

static really_inline
void updateMinMatchOffset(struct RoseContext *tctxt, u64a offset) {
    DEBUG_PRINTF("min match now @%llu, was @%llu\n", offset,
                 tctxt->minMatchOffset);

    assert(offset >= tctxt->minMatchOffset);
    assert(offset >= tctxt->minNonMpvMatchOffset);
    tctxt->minMatchOffset = offset;
    tctxt->minNonMpvMatchOffset = offset;
}

static really_inline
void updateMinMatchOffsetFromMpv(struct RoseContext *tctxt, u64a offset) {
    DEBUG_PRINTF("min match now @%llu, was @%llu\n", offset,
                 tctxt->minMatchOffset);

    assert(offset >= tctxt->minMatchOffset);
    assert(tctxt->minNonMpvMatchOffset >= tctxt->minMatchOffset);
    tctxt->minMatchOffset = offset;
    tctxt->minNonMpvMatchOffset = MAX(tctxt->minNonMpvMatchOffset, offset);
}
#endif
