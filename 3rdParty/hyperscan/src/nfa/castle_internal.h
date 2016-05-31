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
 * \brief Castle: multi-tenant repeat engine, data structures.
 */

#ifndef NFA_CASTLE_INTERNAL_H
#define NFA_CASTLE_INTERNAL_H

#include "ue2common.h"
#include "repeat_internal.h"

struct SubCastle {
    ReportID report;        //!< report to raise on match
    u32 fullStateOffset;    //!< offset within full state (scratch)
    u32 streamStateOffset;  //!< offset within stream state
    u32 repeatInfoOffset;   //!< offset of RepeatInfo structure
                            //   relative to the start of SubCastle
    char exclusive;         //!< exclusive info of this SubCastle
};

#define CASTLE_DOT 0
#define CASTLE_VERM 1
#define CASTLE_NVERM 2
#define CASTLE_SHUFTI 3
#define CASTLE_TRUFFLE 4

/**
 * \brief Castle engine structure.
 *
 * A Castle is a collection of repeats that all share the same character
 * reachability.
 *
 * The whole engine is laid out in memory as:
 *
 * - struct NFA
 * - struct Castle
 * - struct SubCastle[numRepeats]
 * - tables for sparse model repeats
 *
 * Castle stores an "active repeats" multibit in stream state, followed by the
 * packed repeat state for each SubCastle. If all SubCastles are mutual
 * exclusive, we store current active SubCastle id instead of "active repeats"
 * multibit in stream state. If there are both exclusive and non-exclusive
 * SubCastle groups, we use an active id for the exclusive group and a multibit
 * for the non-exclusive group.
 *
 * In full state (stored in scratch space) it stores a temporary multibit over
 * the repeats (used by \ref castleMatchLoop), followed by the repeat control
 * blocks for each SubCastle. If all SubCastles are mutual exclusive, we only
 * need to store the repeat control blocks for each SubCastle.
 */
struct ALIGN_AVX_DIRECTIVE Castle {
    u32 numRepeats;
    u8 type; //!< tells us which scanning mechanism (below) to use
    char exclusive; //!< tells us if there are mutual exclusive SubCastles
    char pureExclusive; //!< tells us if all SubCastles are mutual exclusive
    u8 activeIdxSize; //!< number of bytes in stream state to store
                      // active SubCastle id for exclusive mode
    union {
        struct {
            char c;
        } verm;
        struct {
            m128 mask_lo;
            m128 mask_hi;
        } shuf;
        struct {
            m128 mask1;
            m128 mask2;
        } truffle;
    } u;
};

#endif // NFA_CASTLE_INTERNAL_H
