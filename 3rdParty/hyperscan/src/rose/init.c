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

#include "init.h"
#include "match.h"
#include "runtime.h"
#include "scratch.h"
#include "rose.h"
#include "rose_common.h"
#include "rose_internal.h"
#include "ue2common.h"
#include "nfa/mcclellan.h"
#include "nfa/nfa_api_util.h"
#include "nfa/nfa_internal.h"
#include "sidecar/sidecar.h"
#include "sidecar/sidecar_internal.h"
#include "util/multibit.h"

#include <string.h>

static really_inline
void init_rstate(const struct RoseEngine *t, u8 *state) {
    // Set runtime state: initial depth is 1 and we take our initial groups
    // from the RoseEngine.
    DEBUG_PRINTF("setting initial groups to 0x%016llx\n", t->initialGroups);
    struct RoseRuntimeState *rstate = getRuntimeState(state);
    rstate->stored_depth = 1;
    storeGroups(t, state, t->initialGroups);
    rstate->flags = 0;
    rstate->broken = NOT_BROKEN;
}

static really_inline
void init_sidecar(const struct RoseEngine *t, u8 *state) {
    assert(getSLiteralMatcher(t));

    struct sidecar_enabled *enabled_state
        = (struct sidecar_enabled *)(state + t->stateOffsets.sidecar);

    DEBUG_PRINTF("welcome to the sidecar\n");
    assert(t->initSideEnableOffset);
    // We have to enable some sidecar literals
    const char *template = (const char *)t + t->initSideEnableOffset;

    memcpy(enabled_state, template, t->stateOffsets.sidecar_size);
}

static really_inline
void init_outfixes(const struct RoseEngine *t, u8 *state) {
    /* The active leaf array has been init'ed by the scatter with outfix
     * bits set on */

    // Init the NFA state for each outfix.
    for (u32 qi = t->outfixBeginQueue; qi < t->outfixEndQueue; qi++) {
        const struct NfaInfo *info = getNfaInfoByQueue(t, qi);
        const struct NFA *nfa = getNfaByInfo(t, info);
        nfaInitCompressedState(nfa, 0, state + info->stateOffset,
                               0 /* assume NUL at start */);
    }

    if (t->initMpvNfa != MO_INVALID_IDX) {
        const struct NfaInfo *info = getNfaInfoByQueue(t, t->initMpvNfa);
        const struct NFA *nfa = getNfaByInfo(t, info);
        nfaInitCompressedState(nfa, 0, state + info->stateOffset,
                               0 /* assume NUL at start */);
        mmbit_set(getActiveLeafArray(t, state), t->activeArrayCount,
                  t->initMpvNfa);
    }
}

void roseInitState(const struct RoseEngine *t, u8 *state) {
    assert(t);
    assert(state);

    DEBUG_PRINTF("init for Rose %p with %u roles (%u with state indices)\n",
                 t, t->roleCount, t->rolesWithStateCount);

    // Rose is guaranteed 8-aligned state
    assert(ISALIGNED_N(state, 8));

    init_rstate(t, state);

    // Init the sidecar state
    if (t->smatcherOffset) {
        init_sidecar(t, state);
    }

    init_state(t, state);
    init_outfixes(t, state);

    // Clear the floating matcher state, if any.
    DEBUG_PRINTF("clearing %u bytes of floating matcher state\n",
                 t->floatingStreamState);
    memset(getFloatingMatcherState(t, state), 0, t->floatingStreamState);
}
