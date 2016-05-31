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
 * \brief Build code for Haig SOM DFA.
 */
#include "ng_haig.h"

#include "grey.h"
#include "nfa/goughcompile.h"
#include "ng_holder.h"
#include "ng_mcclellan_internal.h"
#include "ng_restructuring.h"
#include "ng_som_util.h"
#include "ng_squash.h"
#include "ng_util.h"
#include "util/bitfield.h"
#include "util/container.h"
#include "util/determinise.h"
#include "util/graph_range.h"
#include "util/make_unique.h"
#include "util/ue2_containers.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>
#include <boost/dynamic_bitset.hpp>

using namespace std;
using boost::dynamic_bitset;

namespace ue2 {

#define NFA_STATE_LIMIT 256

#define HAIG_MAX_NFA_STATE 600
#define HAIG_MAX_LIVE_SOM_SLOTS 32

namespace {
struct haig_too_wide {
};

template<typename stateset>
static
void populateInit(const NGHolder &g,
                  const ue2::unordered_map<NFAVertex, u32> &state_ids,
                  stateset *init, stateset *initDS,
                  vector<NFAVertex> *v_by_index) {
    DEBUG_PRINTF("graph kind: %u\n", (int)g.kind);
    for (auto v : vertices_range(g)) {
        u32 v_index = g[v].index;
        if (state_ids.at(v) == NO_STATE) {
            continue;
        }

        if (is_any_start(v, g)) {
            init->set(v_index);
            if (hasSelfLoop(v, g) || is_triggered(g)) {
                DEBUG_PRINTF("setting %u\n", v_index);
                initDS->set(v_index);
            }
        }
        assert(v_index < init->size());
    }

    v_by_index->clear();
    v_by_index->resize(num_vertices(g), NFAGraph::null_vertex());

    for (auto v : vertices_range(g)) {
        u32 v_index = g[v].index;
        assert((*v_by_index)[v_index] == NFAGraph::null_vertex());
        (*v_by_index)[v_index] = v;
    }
}

template<typename StateSet>
void populateAccepts(const NGHolder &g, StateSet *accept, StateSet *acceptEod) {
    for (auto v : inv_adjacent_vertices_range(g.accept, g)) {
        accept->set(g[v].index);
    }
    for (auto v : inv_adjacent_vertices_range(g.acceptEod, g)) {
        if (v == g.accept) {
            continue;
        }
        acceptEod->set(g[v].index);
    }
}

class Automaton_Base {
protected:
    Automaton_Base(const NGHolder &graph_in,
                   const ue2::unordered_map<NFAVertex, u32> &state_ids_in)
        : graph(graph_in), state_ids(state_ids_in) {
        calculateAlphabet(graph, alpha, unalpha, &alphasize);
        assert(alphasize <= ALPHABET_SIZE);
    }

public:
    static bool canPrune(const flat_set<ReportID> &) { return false; }

    const NGHolder &graph;
    const ue2::unordered_map<NFAVertex, u32> &state_ids;

    array<u16, ALPHABET_SIZE> alpha;
    array<u16, ALPHABET_SIZE> unalpha;
    u16 alphasize;

    set<dstate_id_t> done_a;
    set<dstate_id_t> done_b;

    u16 start_anchored;
    u16 start_floating;
};

class Automaton_Big : public Automaton_Base {
public:
    typedef dynamic_bitset<> StateSet;
    typedef map<StateSet, dstate_id_t> StateMap;

    Automaton_Big(const NGHolder &graph_in,
                  const ue2::unordered_map<NFAVertex, u32> &state_ids_in,
                  som_type som, const vector<vector<CharReach>> &triggers,
                  bool unordered_som)
        : Automaton_Base(graph_in, state_ids_in), numStates(num_vertices(graph)),
          init(numStates), initDS(numStates), squash(numStates),
          accept(numStates), acceptEod(numStates), toppable(numStates),
          dead(numStates) {
        populateInit(graph, state_ids, &init, &initDS, &v_by_index);
        populateAccepts(graph, &accept, &acceptEod);

        start_anchored = DEAD_STATE + 1;
        if (initDS == init) {
            start_floating = start_anchored;
        } else if (initDS.any()) {
            start_floating = start_anchored + 1;
        } else {
            start_floating = DEAD_STATE;
        }

        if (!unordered_som) {
            for (const auto &sq : findSquashers(graph, som)) {
                NFAVertex v = sq.first;
                u32 vert_id = graph[v].index;
                squash.set(vert_id);
                squash_mask[vert_id] = shrinkStateSet(sq.second);
            }
        }

        cr_by_index = populateCR(graph, v_by_index, alpha);
        if (is_triggered(graph)) {
            markToppableStarts(graph, state_ids, false, triggers, &toppable);
        }
    }

private:
    // Convert an NFAStateSet (as used by the squash code) into a StateSet.
    StateSet shrinkStateSet(const NFAStateSet &in) const {
        StateSet out(dead.size());
        for (size_t i = in.find_first(); i != in.npos && i < out.size();
             i = in.find_next(i)) {
            out.set(i);
        }
        return out;
    }

public:
    void transition(const StateSet &in, StateSet *next) {
        transition_graph(*this, v_by_index, in, next);
    }

    const vector<StateSet> initial() {
        vector<StateSet> rv(1, init);
        if (start_floating != DEAD_STATE && start_floating != start_anchored) {
            rv.push_back(initDS);
        }
        return rv;
    }

private:
    void reports_i(const StateSet &in, bool eod, flat_set<ReportID> &rv) {
        StateSet acc = in & (eod ? acceptEod : accept);
        for (size_t i = acc.find_first(); i != StateSet::npos;
             i = acc.find_next(i)) {
            NFAVertex v = v_by_index[i];
            DEBUG_PRINTF("marking report\n");
            const auto &my_reports = graph[v].reports;
            rv.insert(my_reports.begin(), my_reports.end());
        }
    }

public:
    void reports(const StateSet &in, flat_set<ReportID> &rv) {
        reports_i(in, false, rv);
    }
    void reportsEod(const StateSet &in, flat_set<ReportID> &rv) {
        reports_i(in, true, rv);
    }

public:
    u32 numStates;
    vector<NFAVertex> v_by_index;
    vector<CharReach> cr_by_index; /* pre alpha'ed */
    StateSet init;
    StateSet initDS;
    StateSet squash; /* states which allow us to mask out other states */
    StateSet accept;
    StateSet acceptEod;
    StateSet toppable; /* states which are allowed to be on when a top arrives,
                        * triggered dfas only */
    map<u32, StateSet> squash_mask;
    StateSet dead;
};

class Automaton_Graph : public Automaton_Base {
public:
    typedef bitfield<NFA_STATE_LIMIT> StateSet;
    typedef ue2::unordered_map<StateSet, dstate_id_t> StateMap;

    Automaton_Graph(const NGHolder &graph_in,
                    const ue2::unordered_map<NFAVertex, u32> &state_ids_in,
                    som_type som, const vector<vector<CharReach>> &triggers,
                    bool unordered_som)
        : Automaton_Base(graph_in, state_ids_in) {
        populateInit(graph, state_ids, &init, &initDS, &v_by_index);
        populateAccepts(graph, &accept, &acceptEod);

        start_anchored = DEAD_STATE + 1;
        if (initDS == init) {
            start_floating = start_anchored;
        } else if (initDS.any()) {
            start_floating = start_anchored + 1;
        } else {
            start_floating = DEAD_STATE;
        }

        if (!unordered_som) {
            for (const auto &sq : findSquashers(graph, som)) {
                NFAVertex v = sq.first;
                u32 vert_id = graph[v].index;
                squash.set(vert_id);
                squash_mask[vert_id] = shrinkStateSet(sq.second);
            }
        }

        cr_by_index = populateCR(graph, v_by_index, alpha);
        if (is_triggered(graph)) {
            dynamic_bitset<> temp(NFA_STATE_LIMIT);
            markToppableStarts(graph, state_ids, false, triggers, &temp);
            toppable = bitfield<NFA_STATE_LIMIT>(temp);
        }
    }

private:
    // Convert an NFAStateSet (as used by the squash code) into a StateSet.
    StateSet shrinkStateSet(const NFAStateSet &in) const {
        StateSet out;
        for (size_t i = in.find_first(); i != in.npos && i < out.size();
             i = in.find_next(i)) {
            out.set(i);
        }
        return out;
    }

public:
    void transition(const StateSet &in, StateSet *next) {
        transition_graph(*this, v_by_index, in, next);
    }

    const vector<StateSet> initial() {
        vector<StateSet> rv(1, init);
        if (start_floating != DEAD_STATE && start_floating != start_anchored) {
            rv.push_back(initDS);
        }
        return rv;
    }

private:
    void reports_i(const StateSet &in, bool eod, flat_set<ReportID> &rv) {
        StateSet acc = in & (eod ? acceptEod : accept);
        for (size_t i = acc.find_first(); i != StateSet::npos;
             i = acc.find_next(i)) {
            NFAVertex v = v_by_index[i];
            DEBUG_PRINTF("marking report\n");
            const auto &my_reports = graph[v].reports;
            rv.insert(my_reports.begin(), my_reports.end());
        }
    }

public:
    void reports(const StateSet &in, flat_set<ReportID> &rv) {
        reports_i(in, false, rv);
    }
    void reportsEod(const StateSet &in, flat_set<ReportID> &rv) {
        reports_i(in, true, rv);
    }

public:
    vector<NFAVertex> v_by_index;
    vector<CharReach> cr_by_index; /* pre alpha'ed */
    StateSet init;
    StateSet initDS;
    StateSet squash; /* states which allow us to mask out other states */
    StateSet accept;
    StateSet acceptEod;
    StateSet toppable; /* states which are allowed to be on when a top arrives,
                        * triggered dfas only */
    map<u32, StateSet> squash_mask;
    StateSet dead;
};

class Automaton_Haig_Merge {
public:
    typedef vector<u16> StateSet;
    typedef ue2::unordered_map<StateSet, dstate_id_t> StateMap;

    explicit Automaton_Haig_Merge(const vector<const raw_som_dfa *> &in)
        : nfas(in.begin(), in.end()), dead(in.size()) {
        calculateAlphabet();
        populateAsFs();
    }

    void populateAsFs(void) {
        bool fs_same = true;
        bool fs_dead = true;

        as.resize(nfas.size());
        fs.resize(nfas.size());
        for (u32 i = 0; i < nfas.size(); i++) {
            as[i] = nfas[i]->start_anchored;
            fs[i] = nfas[i]->start_floating;

            if (fs[i]) {
                fs_dead = false;
            }

            if (as[i] != fs[i]) {
                fs_same = false;
            }
        }

        start_anchored = DEAD_STATE + 1;
        if (fs_same) {
            start_floating = start_anchored;
        } else if (fs_dead) {
            start_floating = DEAD_STATE;
        } else {
            start_floating = start_anchored + 1;
        }
    }

    void calculateAlphabet(void) {
        DEBUG_PRINTF("calculating alphabet\n");
        vector<CharReach> esets(1, CharReach::dot());

        for (const auto &haig : nfas) {
            DEBUG_PRINTF("...next dfa alphabet\n");
            assert(haig);
            const auto &alpha_remap = haig->alpha_remap;

            for (size_t i = 0; i < esets.size(); i++) {
                assert(esets[i].any());
                if (esets[i].count() == 1) {
                    DEBUG_PRINTF("skipping singleton eq set\n");
                    continue;
                }

                CharReach t;
                u8 leader_s = alpha_remap[esets[i].find_first()];

                DEBUG_PRINTF("checking eq set, leader %02hhx \n", leader_s);

                for (size_t s = esets[i].find_first();
                     s != CharReach::npos; s = esets[i].find_next(s)) {
                    if (alpha_remap[s] != leader_s) {
                        t.set(s);
                    }
                }

                if (t.any() && t != esets[i]) {
                    esets[i] &= ~t;
                    esets.push_back(t);
                }
            }
        }

        alphasize = buildAlphabetFromEquivSets(esets, alpha, unalpha);
    }

    void transition(const StateSet &in, StateSet *next) {
        u16 t[ALPHABET_SIZE];

        for (u32 i = 0; i < alphasize; i++) {
            next[i].resize(nfas.size());
        }

        for (u32 j = 0; j < nfas.size(); j++) {
            getFullTransitionFromState(*nfas[j], in[j], t);
            for (u32 i = 0; i < alphasize; i++) {
                next[i][j]= t[unalpha[i]];
            }
        }
    }

    const vector<StateSet> initial() {
        vector<StateSet> rv(1, as);
        if (start_floating != DEAD_STATE && start_floating != start_anchored) {
            rv.push_back(fs);
        }
        return rv;
    }

private:
    void reports_i(const StateSet &in, flat_set<ReportID> dstate::*r_set,
                   flat_set<ReportID> &r) {
        for (u32 i = 0; i < nfas.size(); i++) {
            const auto &rs = nfas[i]->states[in[i]].*r_set;
            insert(&r, rs);
        }
    }

public:
    void reports(const StateSet &in, flat_set<ReportID> &rv) {
        reports_i(in, &dstate::reports, rv);
    }
    void reportsEod(const StateSet &in, flat_set<ReportID> &rv) {
        reports_i(in, &dstate::reports_eod, rv);
    }

    static bool canPrune(const flat_set<ReportID> &) { return false; }

private:
    vector<const raw_som_dfa *> nfas;
    vector<dstate_id_t> as;
    vector<dstate_id_t> fs;
public:
    array<u16, ALPHABET_SIZE> alpha;
    array<u16, ALPHABET_SIZE> unalpha;
    u16 alphasize;
    StateSet dead;

    u16 start_anchored;
    u16 start_floating;
};
}

enum bslm_mode {
    ONLY_EXISTING,
    INCLUDE_INVALID
};

static
bool is_any_start_inc_virtual(NFAVertex v, const NGHolder &g) {
    return is_virtual_start(v, g) || is_any_start(v, g);
}

static
s32 getSlotID(const NGHolder &g,
              UNUSED const ue2::unordered_map<NFAVertex, u32> &state_ids,
              NFAVertex v) {
    if (is_triggered(g) && v == g.start) {
        assert(state_ids.at(v) != NO_STATE);
    } else if (is_any_start_inc_virtual(v, g)) {
        return CREATE_NEW_SOM;
    }

    return g[v].index;
}

template<typename stateset>
static
void haig_do_preds(const NGHolder &g, const stateset &nfa_states,
                   const vector<NFAVertex> &state_mapping,
                   som_tran_info &preds) {
    for (size_t i = nfa_states.find_first(); i != stateset::npos;
         i = nfa_states.find_next(i)) {
        NFAVertex v = state_mapping[i];
        s32 slot_id = g[v].index;

        DEBUG_PRINTF("d vertex %u\n", g[v].index);
        vector<u32> &out_map = preds[slot_id];
        for (auto u : inv_adjacent_vertices_range(v, g)) {
            out_map.push_back(g[u].index);
        }

        sort(out_map.begin(), out_map.end());
        assert(!out_map.empty() || v == g.start);
    }
}

template<typename stateset>
static
void haig_do_report(const NGHolder &g,
                    const ue2::unordered_map<NFAVertex, u32> &state_ids,
                    NFAVertex accept_v, const stateset &source_nfa_states,
                    const vector<NFAVertex> &state_mapping,
                    set<som_report> &out) {
    for (size_t i = source_nfa_states.find_first(); i != stateset::npos;
         i = source_nfa_states.find_next(i)) {
        NFAVertex v = state_mapping[i];
        if (!edge(v, accept_v, g).second) {
            continue;
        }
        for (ReportID report_id : g[v].reports) {
            out.insert(som_report(report_id, getSlotID(g, state_ids, v)));
        }
    }
}

static
void haig_note_starts(const NGHolder &g, map<u32, u32> *out) {
    if (is_triggered(g)) {
        return;
    }

    DEBUG_PRINTF("seeing who creates new som values\n");

    vector<DepthMinMax> depths = getDistancesFromSOM(g);

    for (auto v : vertices_range(g)) {
        if (is_any_start_inc_virtual(v, g)) {
            DEBUG_PRINTF("%u creates new som value\n", g[v].index);
            out->emplace(g[v].index, 0U);
            continue;
        }

        if (is_any_accept(v, g)) {
            continue;
        }

        const DepthMinMax &d = depths[g[v].index];
        if (d.min == d.max && d.min.is_finite()) {
            DEBUG_PRINTF("%u is fixed at %u\n", g[v].index, (u32)d.min);
            out->emplace(g[v].index, d.min);
        }
    }
}

template<class Auto>
static
bool doHaig(const NGHolder &g,
            const ue2::unordered_map<NFAVertex, u32> &state_ids,
            som_type som, const vector<vector<CharReach>> &triggers,
            bool unordered_som, raw_som_dfa *rdfa) {
    u32 state_limit = HAIG_FINAL_DFA_STATE_LIMIT; /* haig never backs down from
                                                     a fight */
    typedef typename Auto::StateSet StateSet;
    vector<StateSet> nfa_state_map;
    Auto n(g, state_ids, som, triggers, unordered_som);
    try {
        if (determinise(n, rdfa->states, state_limit, &nfa_state_map)) {
            DEBUG_PRINTF("state limit exceeded\n");
            return false;
        }
    } catch (haig_too_wide &) {
        DEBUG_PRINTF("too many live som states\n");
        return false;
    }

    rdfa->start_anchored = n.start_anchored;
    rdfa->start_floating = n.start_floating;
    rdfa->alpha_size = n.alphasize;
    rdfa->alpha_remap = n.alpha;

    rdfa->state_som.reserve(rdfa->states.size());
    for (u32 i = 0; i < rdfa->states.size(); i++) {
        rdfa->state_som.push_back(dstate_som());
        const StateSet &source_states = nfa_state_map[i];
        if (source_states.count() > HAIG_MAX_LIVE_SOM_SLOTS) {
            DEBUG_PRINTF("too many live states\n");
            return false;
        }

        DEBUG_PRINTF("generating som info for %u\n", i);

        haig_do_preds(g, source_states, n.v_by_index,
                      rdfa->state_som.back().preds);

        haig_do_report(g, state_ids, g.accept, source_states, n.v_by_index,
                       rdfa->state_som.back().reports);
        haig_do_report(g, state_ids, g.acceptEod, source_states, n.v_by_index,
                       rdfa->state_som.back().reports_eod);
    }

    haig_note_starts(g, &rdfa->new_som_nfa_states);

    return true;
}

unique_ptr<raw_som_dfa> attemptToBuildHaig(NGHolder &g, som_type som,
                                   u32 somPrecision,
                                   const vector<vector<CharReach> > &triggers,
                                   const Grey &grey, bool unordered_som) {
    assert(is_triggered(g) != triggers.empty());
    assert(!unordered_som || is_triggered(g));

    if (!grey.allowGough) {
        /* must be at least one engine capable of handling raw som dfas */
        return nullptr;
    }

    auto state_ids = numberStates(g);
    dropUnusedStarts(g, state_ids);

    DEBUG_PRINTF("attempting to build haig \n");
    assert(allMatchStatesHaveReports(g));
    assert(hasCorrectlyNumberedVertices(g));

    u32 numStates = num_vertices(g);
    if (numStates > HAIG_MAX_NFA_STATE) {
        DEBUG_PRINTF("giving up... looks too big\n");
        return nullptr;
    }

    auto rdfa = ue2::make_unique<raw_som_dfa>(g.kind, unordered_som, NODE_START,
                                              somPrecision);

    DEBUG_PRINTF("determinising nfa with %u vertices\n", numStates);
    bool rv;
    if (numStates <= NFA_STATE_LIMIT) {
        /* fast path */
        rv = doHaig<Automaton_Graph>(g, state_ids, som, triggers, unordered_som,
                                     rdfa.get());
    } else {
        /* not the fast path */
        rv = doHaig<Automaton_Big>(g, state_ids, som, triggers, unordered_som,
                                   rdfa.get());
    }

    if (!rv) {
        return nullptr;
    }

    DEBUG_PRINTF("determinised, building impl dfa (a,f) = (%hu,%hu)\n",
                 rdfa->start_anchored, rdfa->start_floating);

    assert(rdfa->kind == g.kind);
    return rdfa;
}

static
void haig_merge_do_preds(const vector<const raw_som_dfa *> &dfas,
                         const vector<u32> &per_dfa_adj,
                         const vector<dstate_id_t> &source_nfa_states,
                         som_tran_info &som_tran) {
    for (u32 d = 0; d < dfas.size(); ++d) {
        u32 adj = per_dfa_adj[d];

        const som_tran_info &som_tran_d
            = dfas[d]->state_som[source_nfa_states[d]].preds;
        for (som_tran_info::const_iterator it = som_tran_d.begin();
             it != som_tran_d.end(); ++it) {
            assert(it->first != CREATE_NEW_SOM);
            u32 dest_slot = it->first < N_SPECIALS ? it->first
                                                   : it->first + adj;
            vector<u32> &out = som_tran[dest_slot];

            if (!out.empty()) {
                /* stylised specials already done; it does not matter who builds
                   the preds */
                assert(dest_slot < N_SPECIALS);
                continue;
            }
            for (vector<u32>::const_iterator jt = it->second.begin();
                 jt != it->second.end(); ++jt) {
                if (*jt < N_SPECIALS || *jt == CREATE_NEW_SOM) {
                    out.push_back(*jt);
                } else {
                    out.push_back(*jt + adj);
                }
            }
        }
    }
}

static
void haig_merge_note_starts(const vector<const raw_som_dfa *> &dfas,
                            const vector<u32> &per_dfa_adj,
                            map<u32, u32> *out) {
    for (u32 d = 0; d < dfas.size(); ++d) {
        u32 adj = per_dfa_adj[d];
        const map<u32, u32> &new_soms = dfas[d]->new_som_nfa_states;
        for (map<u32, u32>::const_iterator it = new_soms.begin();
             it != new_soms.end(); ++it) {
            if (it->first < N_SPECIALS) {
                assert(!it->second);
                out->emplace(it->first, 0U);
            } else {
                assert(d + 1 >= per_dfa_adj.size()
                       || it->first + adj < per_dfa_adj[d + 1]);
                out->emplace(it->first + adj, it->second);
            }
        }
    }
}

static never_inline
void haig_merge_do_report(const vector<const raw_som_dfa *> &dfas,
                          const vector<u32> &per_dfa_adj,
                          const vector<dstate_id_t> &source_nfa_states,
                          bool eod, set<som_report> &out) {
    for (u32 d = 0; d < dfas.size(); ++d) {
        u32 adj = per_dfa_adj[d];

        const set<som_report> &reps = eod
            ? dfas[d]->state_som[source_nfa_states[d]].reports_eod
            : dfas[d]->state_som[source_nfa_states[d]].reports;
        for (set<som_report>::const_iterator it = reps.begin();
             it != reps.end(); ++it) {
            u32 slot = it->slot;
            if (slot != CREATE_NEW_SOM && slot >= N_SPECIALS) {
                slot += adj;
            }
            out.insert(som_report(it->report, slot));
        }
    }
}

static
u32 total_slots_used(const raw_som_dfa &rdfa) {
    u32 rv = 0;
    for (vector<dstate_som>::const_iterator it = rdfa.state_som.begin();
         it != rdfa.state_som.end(); ++it) {
        for (som_tran_info::const_iterator jt = it->preds.begin();
             jt != it->preds.end(); ++jt) {
            assert(jt->first != CREATE_NEW_SOM);
            ENSURE_AT_LEAST(&rv, jt->first + 1);
        }
    }
    const map<u32, u32> &new_soms = rdfa.new_som_nfa_states;
    for (map<u32, u32>::const_iterator it = new_soms.begin();
         it != new_soms.end(); ++it) {
        ENSURE_AT_LEAST(&rv, it->first + 1);
    }
    return rv;
}

unique_ptr<raw_som_dfa> attemptToMergeHaig(const vector<const raw_som_dfa *> &dfas,
                                           u32 limit) {
    assert(!dfas.empty());

    Automaton_Haig_Merge n(dfas);

    DEBUG_PRINTF("merging %zu dfas\n", dfas.size());

    bool unordered_som = false;
    for (const auto &haig : dfas) {
        assert(haig);
        assert(haig->kind == dfas.front()->kind);
        unordered_som |= haig->unordered_som_triggers;
        if (haig->states.size() > limit) {
            DEBUG_PRINTF("too many states!\n");
            return nullptr;
        }
    }

    typedef Automaton_Haig_Merge::StateSet StateSet;
    vector<StateSet> nfa_state_map;
    auto rdfa = ue2::make_unique<raw_som_dfa>(dfas[0]->kind, unordered_som,
                                              NODE_START,
                                              dfas[0]->stream_som_loc_width);

    int rv = determinise(n, rdfa->states, limit, &nfa_state_map);
    if (rv) {
        DEBUG_PRINTF("%d:state limit (%u) exceeded\n", rv, limit);
        return nullptr; /* over state limit */
    }

    rdfa->start_anchored = n.start_anchored;
    rdfa->start_floating = n.start_floating;
    rdfa->alpha_size = n.alphasize;
    rdfa->alpha_remap = n.alpha;

    vector<u32> per_dfa_adj;
    u32 curr_adj = 0;
    for (const auto &haig : dfas) {
        per_dfa_adj.push_back(curr_adj);
        curr_adj += total_slots_used(*haig);
        if (curr_adj < per_dfa_adj.back()) {
            /* overflowed our som slot count */
            return nullptr;
        }
    }

    rdfa->state_som.reserve(rdfa->states.size());
    for (u32 i = 0; i < rdfa->states.size(); i++) {
        rdfa->state_som.push_back(dstate_som());
        const vector<dstate_id_t> &source_nfa_states = nfa_state_map[i];
        DEBUG_PRINTF("finishing state %u\n", i);

        haig_merge_do_preds(dfas, per_dfa_adj, source_nfa_states,
                            rdfa->state_som.back().preds);

        if (rdfa->state_som.back().preds.size() > HAIG_MAX_LIVE_SOM_SLOTS) {
            DEBUG_PRINTF("som slot limit exceeded (%zu)\n",
                         rdfa->state_som.back().preds.size());
            return nullptr;
        }

        haig_merge_do_report(dfas, per_dfa_adj, source_nfa_states,
                             false /* not eod */,
                             rdfa->state_som.back().reports);
        haig_merge_do_report(dfas, per_dfa_adj, source_nfa_states,
                             true /* eod */,
                             rdfa->state_som.back().reports_eod);
    }

    haig_merge_note_starts(dfas, per_dfa_adj, &rdfa->new_som_nfa_states);

    DEBUG_PRINTF("merged, building impl dfa (a,f) = (%hu,%hu)\n",
                 rdfa->start_anchored, rdfa->start_floating);

    return rdfa;
}

} // namespace ue2
