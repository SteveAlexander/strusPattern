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

#include "catchup.h"
#include "match.h"
#include "rose.h"
#include "nfa/nfa_rev_api.h"
#include "nfa/mpv.h"
#include "som/som_runtime.h"
#include "util/fatbit.h"

typedef struct queue_match PQ_T;
#define PQ_COMP(pqc_items, a, b) ((pqc_items)[a].loc < (pqc_items)[b].loc)
#define PQ_COMP_B(pqc_items, a, b_fixed) ((pqc_items)[a].loc < (b_fixed).loc)

#include "util/pqueue.h"

static really_inline
int handleReportInternally(struct hs_scratch *scratch, ReportID id,
                           u64a offset) {
    const struct internal_report *ri = getInternalReport(scratch->tctxt.t, id);
    if (ri->type == EXTERNAL_CALLBACK) {
        return 0;
    }
    if (isInternalSomReport(ri)) {
        handleSomInternal(scratch, ri, offset);
        return 1;
    }
    if (ri->type == INTERNAL_ROSE_CHAIN) {
        roseHandleChainMatch(scratch->tctxt.t, id, offset, &scratch->tctxt, 0,
                             1);
        return 1;
    }

    return 0;
}

static really_inline
int handleReportInternallyNoChain(struct hs_scratch *scratch, ReportID id,
                                  u64a offset) {
    const struct internal_report *ri = getInternalReport(scratch->tctxt.t, id);
    if (ri->type == EXTERNAL_CALLBACK) {
        return 0;
    }
    if (isInternalSomReport(ri)) {
        handleSomInternal(scratch, ri, offset);
        return 1;
    }
    if (ri->type == INTERNAL_ROSE_CHAIN) {
        assert(0); /* chained engines cannot trigger other engines */
        return 1;
    }

    return 0;
}

static really_inline
void currentAnchoredMatch(const struct RoseEngine *t,
                          struct RoseContext *tctxt, ReportID *reportId,
                          u64a *end) {
    if (tctxt->curr_anchored_loc == MMB_INVALID) {
        *end = ANCHORED_MATCH_SENTINEL;
        *reportId = ANCHORED_MATCH_SENTINEL;
        DEBUG_PRINTF("curr %u [idx = %u] @%llu\n", *reportId,
                     tctxt->curr_row_offset, *end);
        return;
    }

    *end = tctxt->curr_anchored_loc + t->maxSafeAnchoredDROffset + 1;
    *reportId = getAnchoredMap(t)[tctxt->curr_row_offset];

    DEBUG_PRINTF("curr %u [idx = %u] @%llu\n", *reportId,
                 tctxt->curr_row_offset, *end);
}

static rose_inline
void nextAnchoredMatch(const struct RoseEngine *t, struct RoseContext *tctxt,
                       ReportID *reportId, u64a *end) {
    assert(tctxt->curr_anchored_loc != MMB_INVALID);

    struct hs_scratch *scratch = tctxtToScratch(tctxt);
    u8 **anchoredRows = getAnchoredLog(scratch);

    u32 region_width = t->anchoredMatches;
    u8 *curr_row = anchoredRows[tctxt->curr_anchored_loc];

    tctxt->curr_row_offset = mmbit_iterate(curr_row, region_width,
                                           tctxt->curr_row_offset);
    DEBUG_PRINTF("next %u [idx = %u] @%llu\n", *reportId,
                 tctxt->curr_row_offset, *end);
    if (tctxt->curr_row_offset != MMB_INVALID) {
        *end = tctxt->curr_anchored_loc + t->maxSafeAnchoredDROffset + 1;
        *reportId = getAnchoredMap(t)[tctxt->curr_row_offset];
        return;
    }

    tctxt->curr_anchored_loc = bf64_iterate(scratch->am_log_sum,
                                            tctxt->curr_anchored_loc);

    if (tctxt->curr_anchored_loc == MMB_INVALID) {
        *end = ANCHORED_MATCH_SENTINEL;
        *reportId = ANCHORED_MATCH_SENTINEL;
        return;
    }

    assert(tctxt->curr_anchored_loc < scratch->anchored_region_len);
    curr_row = anchoredRows[tctxt->curr_anchored_loc];

    tctxt->curr_row_offset = mmbit_iterate(curr_row, region_width,
                                           MMB_INVALID);
    assert(tctxt->curr_row_offset != MMB_INVALID);

    *end = tctxt->curr_anchored_loc + t->maxSafeAnchoredDROffset + 1;
    *reportId = getAnchoredMap(t)[tctxt->curr_row_offset];
}

static really_inline
void deactivateQueue(u8 *aa, u32 qi, struct hs_scratch *scratch) {
    u32 aaCount = scratch->tctxt.t->activeArrayCount;
    u32 qCount = scratch->tctxt.t->queueCount;

    /* this is sailing close to the wind with regards to invalidating an
     * iteration. We are saved by the fact that unsetting does not clear the
     * summary bits -> the block under the gun remains valid
     */
    DEBUG_PRINTF("killing off zombie queue %u\n", qi);
    mmbit_unset(aa, aaCount, qi);
    fatbit_unset(scratch->aqa, qCount, qi);
}

static really_inline
void ensureQueueActive(const struct RoseEngine *t, u32 qi, u32 qCount,
                       struct mq *q, struct hs_scratch *scratch) {
    if (!fatbit_set(scratch->aqa, qCount, qi)) {
        DEBUG_PRINTF("initing %u\n", qi);
        initQueue(q, qi, t, &scratch->tctxt);
        loadStreamState(q->nfa, q, 0);
        pushQueueAt(q, 0, MQE_START, 0);
    }
}

static really_inline
void pq_replace_top_with(struct catchup_pq *pq,
                         UNUSED struct hs_scratch *scratch, u32 queue,
                         s64a loc) {
    DEBUG_PRINTF("inserting q%u in pq at %lld\n", queue, loc);
    struct queue_match temp = {
        .queue = queue,
        .loc = (size_t)loc
    };

    assert(loc > 0);
    assert(pq->qm_size);
    assert(loc <= (s64a)scratch->core_info.len);
    pq_replace_top(pq->qm, pq->qm_size, temp);
}

static really_inline
void pq_insert_with(struct catchup_pq *pq,
                    UNUSED struct hs_scratch *scratch, u32 queue, s64a loc) {
    DEBUG_PRINTF("inserting q%u in pq at %lld\n", queue, loc);
    struct queue_match temp = {
        .queue = queue,
        .loc = (size_t)loc
    };

    assert(loc > 0);
    assert(loc <= (s64a)scratch->core_info.len);
    pq_insert(pq->qm, pq->qm_size, temp);
    ++pq->qm_size;
}

static really_inline
void pq_pop_nice(struct catchup_pq *pq) {
    pq_pop(pq->qm, pq->qm_size);
    pq->qm_size--;
}

static really_inline
s64a pq_top_loc(struct catchup_pq *pq) {
    assert(pq->qm_size);
    return (s64a)pq_top(pq->qm)->loc;
}

/* requires that we are the top item on the pq */
static really_inline
hwlmcb_rv_t runExistingNfaToNextMatch(u32 qi, struct mq *q, s64a loc,
                                      struct hs_scratch *scratch, u8 *aa,
                                      char report_curr) {
    assert(pq_top(scratch->catchup_pq.qm)->queue == qi);
    assert(scratch->catchup_pq.qm_size);
    assert(!q->report_current);
    if (report_curr) {
        DEBUG_PRINTF("need to report matches\n");
        q->report_current = 1;
    }

    DEBUG_PRINTF("running queue from %u:%lld to %lld\n", q->cur, q_cur_loc(q),
                 loc);

    assert(q_cur_loc(q) <= loc);

    char alive = nfaQueueExecToMatch(q->nfa, q, loc);

    /* exit via gift shop */
    if (alive == MO_MATCHES_PENDING) {
        /* we have pending matches */
        assert(q_cur_loc(q) + scratch->core_info.buf_offset
               >= scratch->tctxt.minMatchOffset);
        pq_replace_top_with(&scratch->catchup_pq, scratch, qi, q_cur_loc(q));
        return HWLM_CONTINUE_MATCHING;
    } else if (!alive) {
        if (report_curr && can_stop_matching(scratch)) {
            DEBUG_PRINTF("bailing\n");
            return HWLM_TERMINATE_MATCHING;
        }

        deactivateQueue(aa, qi, scratch);
    } else if (q->cur == q->end) {
        DEBUG_PRINTF("queue %u finished, nfa lives\n", qi);
        q->cur = q->end = 0;
        pushQueueAt(q, 0, MQE_START, loc);
    } else {
        DEBUG_PRINTF("queue %u unfinished, nfa lives\n", qi);
        u32 i = 0;
        while (q->cur < q->end) {
            q->items[i] = q->items[q->cur++];
            DEBUG_PRINTF("q[%u] = %u:%lld\n", i, q->items[i].type,
                         q->items[i].location);
            assert(q->items[i].type != MQE_END);
            i++;
        }
        q->cur = 0;
        q->end = i;
    }

    pq_pop_nice(&scratch->catchup_pq);

    return HWLM_CONTINUE_MATCHING;
}

static really_inline
hwlmcb_rv_t runNewNfaToNextMatch(u32 qi, struct mq *q, s64a loc,
                                 struct hs_scratch *scratch, u8 *aa,
                                 s64a report_ok_loc) {
    assert(!q->report_current);
    DEBUG_PRINTF("running queue from %u:%lld to %lld\n", q->cur, q_cur_loc(q),
                 loc);
    DEBUG_PRINTF("min match offset %llu\n", scratch->tctxt.minMatchOffset);

    char alive = 1;

restart:
    alive = nfaQueueExecToMatch(q->nfa, q, loc);

    if (alive == MO_MATCHES_PENDING) {
        DEBUG_PRINTF("we have pending matches at %lld\n", q_cur_loc(q));
        s64a qcl = q_cur_loc(q);

        if (qcl == report_ok_loc) {
            assert(q->cur != q->end); /* the queue shouldn't be empty if there
                                       * are pending matches. */
            q->report_current = 1;
            DEBUG_PRINTF("restarting...\n");
            goto restart;
        }
        assert(qcl + scratch->core_info.buf_offset
               >= scratch->tctxt.minMatchOffset);
        pq_insert_with(&scratch->catchup_pq, scratch, qi, qcl);
    } else if (!alive) {
        if (can_stop_matching(scratch)) {
            DEBUG_PRINTF("bailing\n");
            return HWLM_TERMINATE_MATCHING;
        }

        deactivateQueue(aa, qi, scratch);
    } else if (q->cur == q->end) {
        DEBUG_PRINTF("queue %u finished, nfa lives\n", qi);
        q->cur = q->end = 0;
        pushQueueAt(q, 0, MQE_START, loc);
    } else {
        DEBUG_PRINTF("queue %u unfinished, nfa lives\n", qi);
        u32 i = 0;
        while (q->cur < q->end) {
            q->items[i] = q->items[q->cur++];
            DEBUG_PRINTF("q[%u] = %u:%lld\n", i, q->items[i].type,
                         q->items[i].location);
            assert(q->items[i].type != MQE_END);
            i++;
        }
        q->cur = 0;
        q->end = i;
    }

    return HWLM_CONTINUE_MATCHING;
}

/* for use by mpv (chained) only */
static UNUSED
int roseNfaFinalBlastAdaptor(u64a offset, ReportID id, void *context) {
    struct RoseContext *tctxt = context;
    struct hs_scratch *scratch = tctxtToScratch(tctxt);

    DEBUG_PRINTF("called\n");

    DEBUG_PRINTF("masky got himself a blasted match @%llu id %u !woot!\n",
                 offset, id);
    updateLastMatchOffset(tctxt, offset);

    if (handleReportInternallyNoChain(scratch, id, offset)) {
        return MO_CONTINUE_MATCHING;
    }

    int cb_rv = tctxt->cb(offset, id, tctxt->userCtx);
    if (cb_rv == MO_HALT_MATCHING) {
        return MO_HALT_MATCHING;
    } else if (cb_rv == ROSE_CONTINUE_MATCHING_NO_EXHAUST) {
        return MO_CONTINUE_MATCHING;
    } else {
        assert(cb_rv == MO_CONTINUE_MATCHING);
       return !roseSuffixIsExhausted(tctxt->t, 0,
                                     scratch->core_info.exhaustionVector);
    }
}

/* for use by mpv (chained) only */
static UNUSED
int roseNfaFinalBlastAdaptorNoInternal(u64a offset, ReportID id,
                                       void *context) {
    struct RoseContext *tctxt = context;
    struct hs_scratch *scratch = tctxtToScratch(tctxt);

    DEBUG_PRINTF("called\n");
    /* chained nfas are run under the control of the anchored catchup */

    DEBUG_PRINTF("masky got himself a blasted match @%llu id %u !woot!\n",
                 offset, id);
    updateLastMatchOffset(tctxt, offset);

    int cb_rv = tctxt->cb(offset, id, tctxt->userCtx);
    if (cb_rv == MO_HALT_MATCHING) {
        return MO_HALT_MATCHING;
    } else if (cb_rv == ROSE_CONTINUE_MATCHING_NO_EXHAUST) {
        return MO_CONTINUE_MATCHING;
    } else {
        assert(cb_rv == MO_CONTINUE_MATCHING);
       return !roseSuffixIsExhausted(tctxt->t, 0,
                                     scratch->core_info.exhaustionVector);
    }
}

static really_inline
void ensureEnd(struct mq *q, UNUSED u32 qi, s64a final_loc) {
    DEBUG_PRINTF("ensure MQE_END %lld for queue %u\n", final_loc, qi);
    if (final_loc >= q_last_loc(q)) {
        /* TODO: ensure situation does not arise */
        assert(q_last_type(q) != MQE_END);
        pushQueueNoMerge(q, MQE_END, final_loc);
    }
}

static really_inline
hwlmcb_rv_t add_to_queue(const struct RoseEngine *t, struct mq *queues,
                         u32 qCount, u8 *aa, struct hs_scratch *scratch,
                         s64a loc, u32 qi, s64a report_ok_loc) {
    struct mq *q = queues + qi;
    const struct NfaInfo *info = getNfaInfoByQueue(t, qi);

    if (roseSuffixInfoIsExhausted(t, info,
                                  scratch->core_info.exhaustionVector)) {
        deactivateQueue(aa, qi, scratch);
        return HWLM_CONTINUE_MATCHING;
    }

    ensureQueueActive(t, qi, qCount, q, scratch);

    if (unlikely(loc < q_cur_loc(q))) {
        DEBUG_PRINTF("err loc %lld < location %lld\n", loc, q_cur_loc(q));
        return HWLM_CONTINUE_MATCHING;
    }

    ensureEnd(q, qi, loc);

    return runNewNfaToNextMatch(qi, q, loc, scratch, aa, report_ok_loc);
}

static really_inline
s64a findSecondPlace(struct catchup_pq *pq, s64a loc_limit) {
    assert(pq->qm_size); /* we are still on the pq and we are first place */

    /* we know (*cough* encapsulation) that second place will either be in
     * pq->qm[1] or pq->qm[2] (we are pq->qm[0]) */
    switch (pq->qm_size) {
    case 0:
    case 1:
        return (s64a)loc_limit;
    case 2:
        return MIN((s64a)pq->qm[1].loc, loc_limit);
    default:;
        size_t best = MIN(pq->qm[1].loc, pq->qm[2].loc);
        return MIN((s64a)best, loc_limit);
    }
}

hwlmcb_rv_t roseCatchUpMPV_i(const struct RoseEngine *t, u8 *state, s64a loc,
                             struct hs_scratch *scratch) {
    struct mq *queues = scratch->queues;
    u8 *aa = getActiveLeafArray(t, state);
    UNUSED u32 aaCount = t->activeArrayCount;
    u32 qCount = t->queueCount;

    /* find first match of each pending nfa */
    DEBUG_PRINTF("aa=%p, aaCount=%u\n", aa, aaCount);

    assert(t->outfixBeginQueue == 1);

    u32 qi = 0;
    assert(mmbit_isset(aa, aaCount, 0)); /* caller should have already bailed */

    DEBUG_PRINTF("catching up qi=%u to loc %lld\n", qi, loc);

    struct mq *q = queues + qi;
    const struct NfaInfo *info = getNfaInfoByQueue(t, qi);
    u64a mpv_exec_end = scratch->core_info.buf_offset + loc;
    u64a next_pos_match_loc = 0;

    if (roseSuffixInfoIsExhausted(t, info,
                                  scratch->core_info.exhaustionVector)) {
        deactivateQueue(aa, qi, scratch);
        goto done;
    }

    ensureQueueActive(t, qi, qCount, q, scratch);

    if (unlikely(loc < q_cur_loc(q))) {
        DEBUG_PRINTF("err loc %lld < location %lld\n", loc, q_cur_loc(q));
        goto done;
    }

    ensureEnd(q, qi, loc);

    assert(!q->report_current);

    if (info->only_external) {
        q->cb = roseNfaFinalBlastAdaptorNoInternal;
    } else {
        q->cb = roseNfaFinalBlastAdaptor;
    }
    q->som_cb = NULL;

    DEBUG_PRINTF("queue %u blasting, %u/%u [%lld/%lld]\n",
                  qi, q->cur, q->end, q->items[q->cur].location, loc);

    scratch->tctxt.mpv_inactive = 0;

    /* we know it is going to be an mpv, skip the indirection */
    next_pos_match_loc = nfaExecMpv0_QueueExecRaw(q->nfa, q, loc);
    assert(!q->report_current);

    if (!next_pos_match_loc) { /* 0 means dead */
        DEBUG_PRINTF("mpv is pining for the fjords\n");
        if (can_stop_matching(scratch)) {
            deactivateQueue(aa, qi, scratch);
            return HWLM_TERMINATE_MATCHING;
        }

        next_pos_match_loc = scratch->core_info.len;
        scratch->tctxt.mpv_inactive = 1;
    }

    if (q->cur == q->end) {
        DEBUG_PRINTF("queue %u finished, nfa lives [%lld]\n", qi, loc);
        q->cur = 0;
        q->end = 0;
        pushQueueAt(q, 0, MQE_START, loc);
    } else {
        DEBUG_PRINTF("queue %u not finished, nfa lives [%lld]\n", qi, loc);
    }

done:
    updateMinMatchOffsetFromMpv(&scratch->tctxt, mpv_exec_end);
    scratch->tctxt.next_mpv_offset
        = MAX(next_pos_match_loc + scratch->core_info.buf_offset,
              mpv_exec_end + 1);

    DEBUG_PRINTF("next match loc %lld (off %llu)\n", next_pos_match_loc,
                  scratch->tctxt.next_mpv_offset);
    return can_stop_matching(scratch) ? HWLM_TERMINATE_MATCHING
                                      : HWLM_CONTINUE_MATCHING;
}

static UNUSED
int roseNfaBlastAdaptor(u64a offset, ReportID id, void *context) {
    struct RoseContext *tctxt = context;
    struct hs_scratch *scratch = tctxtToScratch(tctxt);

    const struct internal_report *ri = getInternalReport(scratch->tctxt.t, id);

    DEBUG_PRINTF("called\n");
    if (ri->type != INTERNAL_ROSE_CHAIN) {
        /* INTERNAL_ROSE_CHAIN are not visible externally */
        if (roseCatchUpMPV(tctxt->t, tctxt->state,
                           offset - scratch->core_info.buf_offset, scratch)
            == HWLM_TERMINATE_MATCHING) {
            DEBUG_PRINTF("done\n");
            return MO_HALT_MATCHING;
        }
    }

    DEBUG_PRINTF("masky got himself a blasted match @%llu id %u !woot!\n",
                 offset, id);

    if (handleReportInternally(scratch, id, offset)) {
        return MO_CONTINUE_MATCHING;
    }

    updateLastMatchOffset(tctxt, offset);

    int cb_rv = tctxt->cb(offset, id, tctxt->userCtx);
    if (cb_rv == MO_HALT_MATCHING) {
        return MO_HALT_MATCHING;
    } else if (cb_rv == ROSE_CONTINUE_MATCHING_NO_EXHAUST) {
        return MO_CONTINUE_MATCHING;
    } else {
        assert(cb_rv == MO_CONTINUE_MATCHING);
       return !roseSuffixIsExhausted(tctxt->t, tctxt->curr_qi,
                                     scratch->core_info.exhaustionVector);
    }
}

static UNUSED
int roseNfaBlastAdaptorNoInternal(u64a offset, ReportID id, void *context) {
    struct RoseContext *tctxt = context;
    struct hs_scratch *scratch = tctxtToScratch(tctxt);

    DEBUG_PRINTF("called\n");
    if (roseCatchUpMPV(tctxt->t, tctxt->state,
                       offset - scratch->core_info.buf_offset, scratch)
        == HWLM_TERMINATE_MATCHING) {
        DEBUG_PRINTF("done\n");
        return MO_HALT_MATCHING;
    }

    DEBUG_PRINTF("masky got himself a blasted match @%llu id %u !woot!\n",
                 offset, id);
    updateLastMatchOffset(tctxt, offset);

    int cb_rv = tctxt->cb(offset, id, tctxt->userCtx);
    if (cb_rv == MO_HALT_MATCHING) {
        return MO_HALT_MATCHING;
    } else if (cb_rv == ROSE_CONTINUE_MATCHING_NO_EXHAUST) {
        return MO_CONTINUE_MATCHING;
    } else {
        assert(cb_rv == MO_CONTINUE_MATCHING);
       return !roseSuffixIsExhausted(tctxt->t, tctxt->curr_qi,
                                     scratch->core_info.exhaustionVector);
    }
}

static UNUSED
int roseNfaBlastAdaptorNoChain(u64a offset, ReportID id, void *context) {
    struct RoseContext *tctxt = context;
    struct hs_scratch *scratch = tctxtToScratch(tctxt);

    DEBUG_PRINTF("masky got himself a blasted match @%llu id %u !woot!\n",
                 offset, id);

    updateLastMatchOffset(tctxt, offset);

    if (handleReportInternallyNoChain(scratch, id, offset)) {
        return MO_CONTINUE_MATCHING;
    }

    int cb_rv = tctxt->cb(offset, id, tctxt->userCtx);
    if (cb_rv == MO_HALT_MATCHING) {
        return MO_HALT_MATCHING;
    } else if (cb_rv == ROSE_CONTINUE_MATCHING_NO_EXHAUST) {
        return MO_CONTINUE_MATCHING;
    } else {
        assert(cb_rv == MO_CONTINUE_MATCHING);
       return !roseSuffixIsExhausted(tctxt->t, tctxt->curr_qi,
                                     scratch->core_info.exhaustionVector);
    }
}

static UNUSED
int roseNfaBlastAdaptorNoInternalNoChain(u64a offset, ReportID id,
                                         void *context) {
    struct RoseContext *tctxt = context;
    struct hs_scratch *scratch = tctxtToScratch(tctxt);

    /* chained nfas are run under the control of the anchored catchup */

    DEBUG_PRINTF("masky got himself a blasted match @%llu id %u !woot!\n",
                 offset, id);
    updateLastMatchOffset(tctxt, offset);

    int cb_rv = tctxt->cb(offset, id, tctxt->userCtx);
    if (cb_rv == MO_HALT_MATCHING) {
        return MO_HALT_MATCHING;
    } else if (cb_rv == ROSE_CONTINUE_MATCHING_NO_EXHAUST) {
        return MO_CONTINUE_MATCHING;
    } else {
        assert(cb_rv == MO_CONTINUE_MATCHING);
       return !roseSuffixIsExhausted(tctxt->t, tctxt->curr_qi,
                                     scratch->core_info.exhaustionVector);
    }
}

static UNUSED
int roseNfaBlastSomAdaptor(u64a from_offset, u64a offset, ReportID id,
                           void *context) {
    struct RoseContext *tctxt = context;
    struct hs_scratch *scratch = tctxtToScratch(tctxt);

    DEBUG_PRINTF("called\n");
    if (roseCatchUpMPV(tctxt->t, tctxt->state,
                       offset - scratch->core_info.buf_offset, scratch)
        == HWLM_TERMINATE_MATCHING) {
        DEBUG_PRINTF("roseCatchUpNfas done\n");
        return MO_HALT_MATCHING;
    }

    DEBUG_PRINTF("masky got himself a blasted match @%llu id %u !woot!\n",
                 offset, id);
    updateLastMatchOffset(tctxt, offset);

    /* must be a external report as haig cannot directly participate in chain */
    int cb_rv = tctxt->cb_som(from_offset, offset, id, tctxt->userCtx);
    if (cb_rv == MO_HALT_MATCHING) {
        return MO_HALT_MATCHING;
    } else if (cb_rv == ROSE_CONTINUE_MATCHING_NO_EXHAUST) {
        return MO_CONTINUE_MATCHING;
    } else {
        assert(cb_rv == MO_CONTINUE_MATCHING);
       return !roseSuffixIsExhausted(tctxt->t, tctxt->curr_qi,
                                     scratch->core_info.exhaustionVector);
    }
}

int roseNfaAdaptor(u64a offset, ReportID id, void *context) {
    struct RoseContext *tctxt = context;
    DEBUG_PRINTF("masky got himself a match @%llu id %u !woot!\n", offset, id);

    updateLastMatchOffset(tctxt, offset);

    struct hs_scratch *scratch = tctxtToScratch(tctxt);
    if (handleReportInternally(scratch, id, offset)) {
        return MO_CONTINUE_MATCHING;
    }

    int cb_rv = tctxt->cb(offset, id, tctxt->userCtx);
    return cb_rv;
}

int roseNfaAdaptorNoInternal(u64a offset, ReportID id, void *context) {
    struct RoseContext *tctxt = context;
    DEBUG_PRINTF("masky got himself a match @%llu id %u !woot!\n", offset, id);
    updateLastMatchOffset(tctxt, offset);

    int cb_rv = tctxt->cb(offset, id, tctxt->userCtx);
    return cb_rv;
}

int roseNfaSomAdaptor(u64a from_offset, u64a offset, ReportID id,
                      void *context) {
    struct RoseContext *tctxt = context;
    DEBUG_PRINTF("masky got himself a match @%llu id %u !woot!\n", offset, id);
    updateLastMatchOffset(tctxt, offset);

    /* must be a external report as haig cannot directly participate in chain */
    int cb_rv = tctxt->cb_som(from_offset, offset, id, tctxt->userCtx);
    return cb_rv;
}

static really_inline
char blast_queue(const struct RoseEngine *t, struct hs_scratch *scratch,
                 struct mq *q, u32 qi, s64a to_loc, char report_current) {
    struct RoseContext *tctxt = &scratch->tctxt;
    const struct NfaInfo *info = getNfaInfoByQueue(t, qi);

    tctxt->curr_qi = qi;
    if (has_chained_nfas(t)) {
        if (info->only_external) {
            q->cb = roseNfaBlastAdaptorNoInternal;
        } else {
            q->cb = roseNfaBlastAdaptor;
        }
    } else {
        if (info->only_external) {
            q->cb = roseNfaBlastAdaptorNoInternalNoChain;
        } else {
            q->cb = roseNfaBlastAdaptorNoChain;
        }
    }
    q->report_current = report_current;
    q->som_cb = roseNfaBlastSomAdaptor;
    DEBUG_PRINTF("queue %u blasting, %u/%u [%lld/%lld]\n", qi, q->cur, q->end,
                 q_cur_loc(q), to_loc);
    char alive = nfaQueueExec(q->nfa, q, to_loc);
    if (info->only_external) {
        q->cb = roseNfaAdaptorNoInternal;
    } else {
        q->cb = roseNfaAdaptor;
    }
    q->som_cb = roseNfaSomAdaptor;
    assert(!q->report_current);

    return alive;
}

static really_inline
hwlmcb_rv_t buildSufPQ_final(const struct RoseEngine *t, s64a report_ok_loc,
                             s64a second_place_loc, s64a final_loc,
                             struct hs_scratch *scratch, u8 *aa, u32 a_qi) {
    struct mq *q = scratch->queues + a_qi;
    const struct NfaInfo *info = getNfaInfoByQueue(t, a_qi);
    DEBUG_PRINTF("blasting qi=%u to %lld [final %lld]\n", a_qi, second_place_loc,
                 final_loc);

    if (roseSuffixInfoIsExhausted(t, info,
                                  scratch->core_info.exhaustionVector)) {
        deactivateQueue(aa, a_qi, scratch);
        return HWLM_CONTINUE_MATCHING;
    }

    ensureQueueActive(t, a_qi, t->queueCount, q, scratch);

    if (unlikely(final_loc < q_cur_loc(q))) {
        DEBUG_PRINTF("err loc %lld < location %lld\n", final_loc, q_cur_loc(q));
        return HWLM_CONTINUE_MATCHING;
    }

    ensureEnd(q, a_qi, final_loc);

    char alive = blast_queue(t, scratch, q, a_qi, second_place_loc, 0);

    /* We have three posible outcomes:
     * (1) the nfa died
     * (2) we completed the queue (implies that second_place_loc == final_loc)
     * (3) the queue ran to second_place_loc and stopped. In this case we need
     *     to find the next match location.
     */

    if (!alive) {
        if (can_stop_matching(scratch)) {
            DEBUG_PRINTF("roseCatchUpNfas done as bailing\n");
            return HWLM_TERMINATE_MATCHING;
        }

        deactivateQueue(aa, a_qi, scratch);
    } else if (q->cur == q->end) {
        DEBUG_PRINTF("queue %u finished, nfa lives [%lld]\n", a_qi, final_loc);

        assert(second_place_loc == final_loc);

        q->cur = q->end = 0;
        pushQueueAt(q, 0, MQE_START, final_loc);
    } else {
        DEBUG_PRINTF("queue %u not finished, %u/%u [%lld/%lld]\n", a_qi, q->cur,
                     q->end, q_cur_loc(q), final_loc);
        DEBUG_PRINTF("finding next match location\n");

        assert(second_place_loc < final_loc);
        assert(q_cur_loc(q) >= second_place_loc);

        if (runNewNfaToNextMatch(a_qi, q, final_loc, scratch, aa,  report_ok_loc)
            == HWLM_TERMINATE_MATCHING) {
            DEBUG_PRINTF("roseCatchUpNfas done\n");
            return HWLM_TERMINATE_MATCHING;
        }
    }

    return HWLM_CONTINUE_MATCHING;
}

void streamInitSufPQ(const struct RoseEngine *t, u8 *state,
                     struct hs_scratch *scratch) {
    assert(scratch->catchup_pq.qm_size == 0);
    assert(t->outfixBeginQueue != t->outfixEndQueue);

    DEBUG_PRINTF("initSufPQ: outfixes [%u,%u)\n", t->outfixBeginQueue,
                 t->outfixEndQueue);

    u32 qCount = t->queueCount;
    u8 *aa = getActiveLeafArray(t, state);
    u32 aaCount = t->activeArrayCount;
    struct mq *queues = scratch->queues;
    size_t length = scratch->core_info.len;

    u32 qi = mmbit_iterate_bounded(aa, aaCount, t->outfixBeginQueue,
                                   t->outfixEndQueue);
    for (; qi < t->outfixEndQueue;) {
        DEBUG_PRINTF("adding qi=%u\n", qi);
        struct mq *q = queues + qi;

        ensureQueueActive(t, qi, qCount, q, scratch);
        ensureEnd(q, qi, length);

        char alive = nfaQueueExecToMatch(q->nfa, q, length);

        if (alive == MO_MATCHES_PENDING) {
            DEBUG_PRINTF("we have pending matches at %lld\n", q_cur_loc(q));
            s64a qcl = q_cur_loc(q);

            pq_insert_with(&scratch->catchup_pq, scratch, qi, qcl);
        } else if (!alive) {
            deactivateQueue(aa, qi, scratch);
        } else {
            assert(q->cur == q->end);
            /* TODO: can this be simplified? the nfa will never produce any
             * matches for this block. */
            DEBUG_PRINTF("queue %u finished, nfa lives\n", qi);
            q->cur = q->end = 0;
            pushQueueAt(q, 0, MQE_START, length);
        }

        qi = mmbit_iterate_bounded(aa, aaCount, qi + 1, t->outfixEndQueue);
    }
}

void blockInitSufPQ(const struct RoseEngine *t, u8 *state,
                    struct hs_scratch *scratch, char is_small_block) {
    DEBUG_PRINTF("initSufPQ: outfixes [%u,%u)\n", t->outfixBeginQueue,
                 t->outfixEndQueue);

    assert(scratch->catchup_pq.qm_size == 0);
    assert(t->outfixBeginQueue != t->outfixEndQueue);

    struct mq *queues = scratch->queues;
    u8 *aa = getActiveLeafArray(t, state);
    struct fatbit *aqa = scratch->aqa;
    u32 aaCount = t->activeArrayCount;
    u32 qCount = t->queueCount;
    size_t length = scratch->core_info.len;

    for (u32 qi = t->outfixBeginQueue; qi < t->outfixEndQueue; qi++) {
        const struct NfaInfo *info = getNfaInfoByQueue(t, qi);

        if (is_small_block && info->in_sbmatcher) {
            DEBUG_PRINTF("skip outfix %u as it's in the SB matcher\n", qi);
            continue;
        }

        const struct NFA *nfa = getNfaByInfo(t, info);
        DEBUG_PRINTF("testing minwidth %u > len %zu\n", nfa->minWidth,
                      length);
        size_t len = nfaRevAccelCheck(nfa, scratch->core_info.buf, length);
        if (!len) {
            continue;
        }
        mmbit_set(aa, aaCount, qi);
        fatbit_set(aqa, qCount, qi);
        struct mq *q = queues + qi;
        initQueue(q, qi, t, &scratch->tctxt);
        q->length = len; /* adjust for rev_accel */
        nfaQueueInitState(nfa, q);
        pushQueueAt(q, 0, MQE_START, 0);
        pushQueueAt(q, 1, MQE_TOP, 0);
        pushQueueAt(q, 2, MQE_END, length);

        DEBUG_PRINTF("adding qi=%u to pq\n", qi);

        char alive = nfaQueueExecToMatch(q->nfa, q, length);

        if (alive == MO_MATCHES_PENDING) {
            DEBUG_PRINTF("we have pending matches at %lld\n", q_cur_loc(q));
            s64a qcl = q_cur_loc(q);

            pq_insert_with(&scratch->catchup_pq, scratch, qi, qcl);
        } else if (!alive) {
            deactivateQueue(aa, qi, scratch);
        } else {
            assert(q->cur == q->end);
            /* TODO: can this be simplified? the nfa will never produce any
             * matches for this block. */
            DEBUG_PRINTF("queue %u finished, nfa lives\n", qi);
            q->cur = q->end = 0;
            pushQueueAt(q, 0, MQE_START, length);
        }
    }
}

/**
 * safe_loc is ???
 */
static rose_inline
hwlmcb_rv_t buildSufPQ(const struct RoseEngine *t, u8 *state, s64a safe_loc,
                       s64a final_loc, struct hs_scratch *scratch) {
    assert(scratch->catchup_pq.qm_size <= t->outfixEndQueue);

    struct RoseContext *tctxt = &scratch->tctxt;
    assert(t->activeArrayCount);

    assert(scratch->core_info.buf_offset + final_loc
           > tctxt->minNonMpvMatchOffset);
    DEBUG_PRINTF("buildSufPQ final loc %lld (safe %lld)\n", final_loc,
                 safe_loc);
    assert(safe_loc <= final_loc);

    u8 *aa = getActiveLeafArray(t, state);
    u32 aaCount = t->activeArrayCount;

    /* find first match of each pending nfa */
    DEBUG_PRINTF("aa=%p, aaCount=%u\n", aa, aaCount);

    /* Note: mpv MUST not participate in the main priority queue as
     * they may have events pushed on during this process which may be before
     * the catch up point. Outfixes are remain in the pq between catchup events
     * as they never have any incoming events to worry about.
     */
    if (aaCount == t->outfixEndQueue) {
        return HWLM_CONTINUE_MATCHING;
    }

    DEBUG_PRINTF("mib %u/%u\n", t->outfixBeginQueue, aaCount);

    u32 a_qi = mmbit_iterate_bounded(aa, aaCount, t->outfixEndQueue, aaCount);

    if (a_qi == MMB_INVALID) {
        return HWLM_CONTINUE_MATCHING;
    }

    s64a report_ok_loc = tctxt->minNonMpvMatchOffset + 1
        - scratch->core_info.buf_offset;

    hwlmcb_rv_t rv = roseCatchUpMPV(tctxt->t, state, report_ok_loc, scratch);
    if (rv != HWLM_CONTINUE_MATCHING) {
        return rv;
    }

    while (a_qi != MMB_INVALID) {
        DEBUG_PRINTF("catching up qi=%u to %lld\n", a_qi, final_loc);
        u32 n_qi = mmbit_iterate(aa, aaCount, a_qi);

        s64a second_place_loc
            = scratch->catchup_pq.qm_size ? pq_top_loc(&scratch->catchup_pq)
                                          : safe_loc;
        second_place_loc = MIN(second_place_loc, safe_loc);
        if (n_qi == MMB_INVALID && report_ok_loc < second_place_loc) {
            if (buildSufPQ_final(t, report_ok_loc, second_place_loc, final_loc,
                                 scratch, aa, a_qi)
                == HWLM_TERMINATE_MATCHING) {
                return HWLM_TERMINATE_MATCHING;
            }
            break;
        }

        if (add_to_queue(t, scratch->queues, t->queueCount, aa, scratch,
                         final_loc, a_qi, report_ok_loc)
            == HWLM_TERMINATE_MATCHING) {
            DEBUG_PRINTF("roseCatchUpNfas done\n");
            return HWLM_TERMINATE_MATCHING;
        }

        a_qi = n_qi;
    }

    DEBUG_PRINTF("PQ BUILD %u items\n", scratch->catchup_pq.qm_size);
    return HWLM_CONTINUE_MATCHING;
}

static never_inline
hwlmcb_rv_t roseCatchUpNfas(const struct RoseEngine *t, u8 *state, s64a loc,
                            s64a final_loc, struct hs_scratch *scratch) {
    struct RoseContext *tctxt = &scratch->tctxt;
    assert(t->activeArrayCount);

    assert(scratch->core_info.buf_offset + loc >= tctxt->minNonMpvMatchOffset);
    DEBUG_PRINTF("roseCatchUpNfas %lld/%lld\n", loc, final_loc);
    DEBUG_PRINTF("min non mpv match offset %llu\n",
                 scratch->tctxt.minNonMpvMatchOffset);

    struct mq *queues = scratch->queues;
    u8 *aa = getActiveLeafArray(t, state);

    /* fire off earliest nfa match and catchup anchored matches to that point */
    while (scratch->catchup_pq.qm_size) {
        s64a match_loc = pq_top_loc(&scratch->catchup_pq);
        u32 qi = pq_top(scratch->catchup_pq.qm)->queue;

        DEBUG_PRINTF("winrar q%u@%lld loc %lld\n", qi, match_loc, loc);
        assert(match_loc + scratch->core_info.buf_offset
               >= scratch->tctxt.minNonMpvMatchOffset);

        if (match_loc > loc) {
            /* we have processed all the matches at or before rose's current
             * location; only things remaining on the pq should be outfixes. */
            DEBUG_PRINTF("saving for later\n");
            goto exit;
        }

        /* catch up char matches to this point */
        if (roseCatchUpMPV(t, state, match_loc, scratch)
            == HWLM_TERMINATE_MATCHING) {
            DEBUG_PRINTF("roseCatchUpNfas done\n");
            return HWLM_TERMINATE_MATCHING;
        }

        assert(match_loc + scratch->core_info.buf_offset
               >= scratch->tctxt.minNonMpvMatchOffset);

        struct mq *q = queues + qi;

        /* outfixes must be advanced all the way as they persist in the pq
         * between catchup events */
        s64a q_final_loc = qi >= t->outfixEndQueue ? final_loc
                                                 : (s64a)scratch->core_info.len;

        /* fire nfa matches, and find next place this nfa match */
        DEBUG_PRINTF("reporting matches %u@%llu [q->cur %u/%u]\n", qi,
                     match_loc, q->cur, q->end);

        /* we then need to catch this nfa up to next earliest nfa match. These
         * matches can be fired directly from the callback. The callback needs
         * to ensure that the anchored matches remain in sync though */
        s64a second_place_loc = findSecondPlace(&scratch->catchup_pq, loc);
        DEBUG_PRINTF("second place %lld loc %lld\n", second_place_loc, loc);

        if (second_place_loc == q_cur_loc(q)) {
            if (runExistingNfaToNextMatch(qi, q, q_final_loc, scratch, aa, 1)
                == HWLM_TERMINATE_MATCHING) {
                return HWLM_TERMINATE_MATCHING;
            }
            continue;
        }

        char alive = blast_queue(t, scratch, q, qi, second_place_loc, 1);

        if (!alive) {
            if (can_stop_matching(scratch)) {
                DEBUG_PRINTF("roseCatchUpNfas done as bailing\n");
                return HWLM_TERMINATE_MATCHING;
            }

            deactivateQueue(aa, qi, scratch);
            pq_pop_nice(&scratch->catchup_pq);
        } else if (q->cur == q->end) {
            DEBUG_PRINTF("queue %u finished, nfa lives [%lld]\n", qi, loc);
            q->cur = q->end = 0;
            pushQueueAt(q, 0, MQE_START, loc);
            pq_pop_nice(&scratch->catchup_pq);
        } else if (second_place_loc == q_final_loc) {
            DEBUG_PRINTF("queue %u on hold\n", qi);
            pq_pop_nice(&scratch->catchup_pq);
            break;
        } else {
            DEBUG_PRINTF("queue %u not finished, %u/%u [%lld/%lld]\n",
                          qi, q->cur, q->end, q->items[q->cur].location, loc);
            runExistingNfaToNextMatch(qi, q, q_final_loc, scratch, aa, 0);
        }
    }
exit:;
    tctxt->minNonMpvMatchOffset = scratch->core_info.buf_offset + loc;
    DEBUG_PRINTF("roseCatchUpNfas done\n");
    return HWLM_CONTINUE_MATCHING;
}

static really_inline
hwlmcb_rv_t roseCatchUpNfasAndMpv(const struct RoseEngine *t, u8 *state,
                                  s64a loc, s64a final_loc,
                                  struct hs_scratch *scratch) {
    hwlmcb_rv_t rv = roseCatchUpNfas(t, state, loc, final_loc, scratch);

    if (rv != HWLM_CONTINUE_MATCHING) {
        return rv;
    }

    return roseCatchUpMPV(t, state, loc, scratch);
}


static really_inline
hwlmcb_rv_t roseCatchUpAll_i(s64a loc, struct hs_scratch *scratch,
                             char do_full_mpv) {
    assert(scratch->tctxt.t->activeArrayCount); /* otherwise use
                                                 * roseCatchUpAnchoredOnly */
    struct RoseContext *tctxt = &scratch->tctxt;
    u64a current_offset = scratch->core_info.buf_offset + loc;

    u64a anchored_end;
    ReportID anchored_report;
    currentAnchoredMatch(tctxt->t, tctxt, &anchored_report, &anchored_end);

    DEBUG_PRINTF("am current_offset %llu\n", current_offset);
    DEBUG_PRINTF("min match offset %llu\n", scratch->tctxt.minMatchOffset);
    DEBUG_PRINTF("min non mpv match offset %llu\n",
                 scratch->tctxt.minNonMpvMatchOffset);

    assert(current_offset > tctxt->minMatchOffset);
    assert(anchored_end != ANCHORED_MATCH_SENTINEL);

    hwlmcb_rv_t rv = buildSufPQ(tctxt->t, tctxt->state,
                                anchored_end - scratch->core_info.buf_offset,
                                loc, scratch);
    if (rv != HWLM_CONTINUE_MATCHING) {
        return rv;
    }

    /* buildSufPQ may have caught only part of the pq upto anchored_end */
    rv = roseCatchUpNfas(tctxt->t, tctxt->state,
                        anchored_end - scratch->core_info.buf_offset, loc,
                        scratch);

    if (rv != HWLM_CONTINUE_MATCHING) {
        return rv;
    }

    while (anchored_report != MO_INVALID_IDX
           && anchored_end <= current_offset) {
        if (anchored_end != tctxt->minMatchOffset) {
            rv = roseCatchUpNfasAndMpv(tctxt->t, tctxt->state,
                                  anchored_end - scratch->core_info.buf_offset,
                                  loc, scratch);
            if (rv != HWLM_CONTINUE_MATCHING) {
                DEBUG_PRINTF("halting\n");
                return rv;
            }
        }

        assert(anchored_end == tctxt->minMatchOffset);
        updateLastMatchOffset(tctxt, anchored_end);

        if (handleReportInternally(scratch, anchored_report, anchored_end)) {
            goto next;
        }

        if (tctxt->cb(anchored_end, anchored_report, tctxt->userCtx)
            == MO_HALT_MATCHING) {
            DEBUG_PRINTF("termination requested\n");
            return HWLM_TERMINATE_MATCHING;
        }
    next:
         nextAnchoredMatch(tctxt->t, tctxt, &anchored_report, &anchored_end);
         DEBUG_PRINTF("catch up %u %llu\n", anchored_report, anchored_end);
    }

    if (current_offset == tctxt->minMatchOffset) {
        DEBUG_PRINTF("caught up\n");
        assert(scratch->catchup_pq.qm_size <= tctxt->t->outfixEndQueue);
        return HWLM_CONTINUE_MATCHING;
    }

    rv = roseCatchUpNfas(tctxt->t, tctxt->state, loc, loc, scratch);

    if (rv != HWLM_CONTINUE_MATCHING) {
        return rv;
    }

    assert(scratch->catchup_pq.qm_size <= tctxt->t->outfixEndQueue
           || rv == HWLM_TERMINATE_MATCHING);

    if (do_full_mpv) {
        /* finish off any outstanding chained matches */
        rv = roseCatchUpMPV(tctxt->t, tctxt->state, loc, scratch);
    }

    DEBUG_PRINTF("catchup all done %llu\n", current_offset);

    return rv;
}

hwlmcb_rv_t roseCatchUpAll(s64a loc, struct hs_scratch *scratch) {
    return roseCatchUpAll_i(loc, scratch, 1);
}

hwlmcb_rv_t roseCatchUpAnchoredAndSuf(s64a loc, struct hs_scratch *scratch) {
    return roseCatchUpAll_i(loc, scratch, 0);
}

hwlmcb_rv_t roseCatchUpSufAndChains(s64a loc, struct hs_scratch *scratch) {
    /* just need suf/outfixes and mpv */
    DEBUG_PRINTF("loc %lld mnmmo %llu mmo %llu\n", loc,
                 scratch->tctxt.minNonMpvMatchOffset,
                 scratch->tctxt.minMatchOffset);
    assert(scratch->core_info.buf_offset + loc
           > scratch->tctxt.minNonMpvMatchOffset);

    hwlmcb_rv_t rv = buildSufPQ(scratch->tctxt.t, scratch->tctxt.state, loc,
                                loc, scratch);
    if (rv != HWLM_CONTINUE_MATCHING) {
        return rv;
    }

    rv = roseCatchUpNfas(scratch->tctxt.t, scratch->tctxt.state, loc, loc,
                         scratch);

    if (rv != HWLM_CONTINUE_MATCHING) {
        return rv;
    }

    rv = roseCatchUpMPV(scratch->tctxt.t, scratch->tctxt.state, loc, scratch);

    assert(rv != HWLM_CONTINUE_MATCHING
           || scratch->catchup_pq.qm_size <= scratch->tctxt.t->outfixEndQueue);

    return rv;
}

hwlmcb_rv_t roseCatchUpSuf(s64a loc, struct hs_scratch *scratch) {
    /* just need suf/outfixes. mpv will be caught up only to last reported
     * external match */
    assert(scratch->core_info.buf_offset + loc
           > scratch->tctxt.minNonMpvMatchOffset);

    hwlmcb_rv_t rv = buildSufPQ(scratch->tctxt.t, scratch->tctxt.state, loc,
                                loc, scratch);
    if (rv != HWLM_CONTINUE_MATCHING) {
        return rv;
    }

    rv = roseCatchUpNfas(scratch->tctxt.t, scratch->tctxt.state, loc, loc,
                         scratch);
    assert(rv != HWLM_CONTINUE_MATCHING
           || scratch->catchup_pq.qm_size <= scratch->tctxt.t->outfixEndQueue);

    return rv;
}

hwlmcb_rv_t roseCatchUpAnchoredOnly(s64a loc, struct hs_scratch *scratch) {
    struct RoseContext *tctxt = &scratch->tctxt;

    assert(!tctxt->t->activeArrayCount); /* otherwise use roseCatchUpAll */

    u64a current_offset = scratch->core_info.buf_offset + loc;
    u64a anchored_end;
    ReportID anchored_report;
    currentAnchoredMatch(tctxt->t, tctxt, &anchored_report, &anchored_end);

    DEBUG_PRINTF("am current_offset %llu\n", current_offset);

    assert(current_offset > tctxt->minMatchOffset);

    while (anchored_report != MO_INVALID_IDX
           && anchored_end <= current_offset) {
        updateLastMatchOffset(tctxt, anchored_end);

        /* as we require that there are no leaf nfas - there must be no nfa */
        if (handleReportInternallyNoChain(scratch, anchored_report,
                                          anchored_end)) {
            goto next;
        }

        if (tctxt->cb(anchored_end, anchored_report, tctxt->userCtx)
                        == MO_HALT_MATCHING) {
            DEBUG_PRINTF("termination requested\n");
            return HWLM_TERMINATE_MATCHING;
        }
    next:
        nextAnchoredMatch(tctxt->t, tctxt, &anchored_report, &anchored_end);
        DEBUG_PRINTF("catch up %u %llu\n", anchored_report, anchored_end);
    }

    updateMinMatchOffset(tctxt, current_offset);
    return HWLM_CONTINUE_MATCHING;
}
