#include "divergence.h"

#include <print>

#include <mim/driver.h>
#include <mim/lam.h>
#include <mim/tuple.h>
#include <mim/world.h>

namespace mim {

namespace {

Lam* isa_block(const Def* def) {
    if (auto lam = Lam::isa_mut_basicblock(def); lam && lam->is_set()) return lam;
    return nullptr;
}

/// return continuation of a function
bool isa_exit(const Def* def) { return !def->isa_mut() && Pi::isa_basicblock(def->type()); }

bool isa_node(const Def* def) { return isa_block(def) || isa_exit(def); }

bool contains(const DefVec& v, const Def* def) { return std::ranges::find(v, def) != v.end(); }

} // namespace

void Divergence::run_to_fp() {
    auto max_iters = driver().flags().max_fp_iters;
    uint32_t i     = 0;
    while (true) {
        if (i >= max_iters) error("phase `{}` did not reach a fixed point after {} iterations", name(), max_iters);
        reset();
        run();
        if (!todo()) break;
        i++;
    }
}

// ==== jump graph ====

void Divergence::prepare() {
    if (!graphs_.empty()) return; // the jump graph is build it once

    for (auto mut : world().externals().muts())
        if (auto lam = mut->isa_mut<Lam>(); lam && lam->is_set()) {
            auto& graph = graphs_.emplace_back();
            graph.root  = lam;
            build(graph);
        }
}

void Divergence::build(ControlGraph& g) {
    DefMap<nodeStates> state;
    DefVec post;
    LamMap<DefVec> split2sources;

    auto add_edge = [&](Lam* q, const Def* p, const Def* arg, DefVec& succs) {
        g.succs[q].emplace_back(p);
        g.preds[p].emplace_back(q);
        if (arg) g.incoming[p].emplace_back(arg);
        succs.emplace_back(p);
    };

    auto succs_of = [&](Lam* q) {
        DefVec succs;
        auto app = q->body()->isa<App>();
        if (!app) return succs;

        auto dispatch = Dispatch(app);
        auto targets  = dispatch ? dispatch.tuple()->projs(dispatch.num_targets()) : DefVec();
        if (dispatch && std::ranges::all_of(targets, isa_node)) {
            DefVec sources;
            for (size_t i = 0, n = dispatch.num_targets(); i != n; ++i)
                if (auto t = dispatch.target(i); !contains(sources, t)) {
                    sources.emplace_back(t);
                    add_edge(q, t, dispatch.arg(), succs);
                }
            if (sources.size() > 1) {
                g.splits[q]      = dispatch.index();
                split2sources[q] = std::move(sources);
            }
        } else if (isa_node(app->callee())) {
            add_edge(q, app->callee(), app->arg(), succs); // plain jump
        } else {
            // Opaque call
            for (auto arg : app->args())
                if (isa_node(arg)) {
                    g.opaque_conts.emplace(arg);
                    add_edge(q, arg, nullptr, succs);
                }
        }
        return succs;
    };

    // record the graph, its post order, and whether it contains a cycle.
    auto dfs = [&](this auto&& self, const Def* n) -> void {
        state[n]       = Active;
        node2graph_[n] = &g;
        if (auto q = isa_block(n); q || n == g.root) {
            for (auto s : succs_of(q ? q : g.root))
                if (auto i = state.find(s); i == state.end())
                    self(s);
                else if (i->second == Active)
                    g.cyclic = true;
        }
        state[n] = Done;
        post.emplace_back(n);
    };
    dfs(g.root);
    g.topo.assign(post.rbegin(), post.rend());

    // Opaque results are the sources of divergence.
    for (auto n : g.topo)
        if (auto lam = isa_block(n); lam && g.opaque_conts.contains(n))
            for (size_t i = 0, e = lam->num_tvars(); i != e; ++i)
                pin_tvar(lam, i);

    // TODO: cycles - run it on the head-rewired graph instead.
    if (g.cyclic) return;

    for (auto n : g.topo)
        if (auto s = isa_block(n); s && split2sources.contains(s)) label_disjoint(g, s, split2sources[s]);
}

/// Algorithm 1: labels every node reachable from @p sources with the earliest node `p*` that lies on
/// all paths from @p sources to it.
/// A node that ends up labeled with itself is the end of two disjoint paths - i.e., @p split is relevant for it.

void Divergence::label_disjoint(ControlGraph& g, Lam* split, const DefVec& sources) {
    DefMap<const Def*> label;
    for (auto p : g.topo) {
        DefVec in;
        if (contains(sources, p)) in.emplace_back(p);
        if (auto i = g.preds.find(p); i != g.preds.end())
            for (auto q : i->second)
                if (auto l = label.find(q); l != label.end()) in.emplace_back(l->second);

        if (in.empty()) continue; // not reachable from sources
        bool same = std::ranges::all_of(in, [&](const Def* l) { return l == in.front(); });
        label[p]  = same ? in.front() : p;
        if (!same) g.sync_deps[p].emplace_back(g.splits[split]);
    }
}

void Divergence::pin_tvar(Lam* lam, size_t i) {
    pin(lam->tvar(i));
    // TODO: marking the whole Var as varying if any of its projections is, is conservative.
    if (lam->num_tvars() > 1) pin(lam->var());
}

const Def* Divergence::rewrite_imm(const Def* def) {
    auto res = mim::Analysis::rewrite_imm(def);

    // Lam::tvar%s carry their own facts
    if (auto extract = def->isa<Extract>(); extract && extract->tuple()->isa<Var>() && Lit::isa(extract->index()))
        return res;

    if (auto app = def->isa<App>(); app && Pi::isa_cn(app->callee()->type())) return res;

    if (std::ranges::any_of(def->ops(), [&](const Def* op) { return is_varying(op); })) pin(def);
    return res;
}

const Def* Divergence::rewrite_imm_App(const App* app) {
    auto res = mim::Analysis::rewrite_imm_App(app);

    if (auto dispatch = Dispatch(app)) {
        for (size_t i = 0, n = dispatch.num_targets(); i != n; ++i)
            if (auto lam = isa_block(dispatch.target(i))) join(lam);
    } else if (auto lam = isa_block(app->callee())) {
        join(lam);
    } else if (app->callee()->isa_mut<Lam>() && !Pi::isa_cn(app->callee()->type())) {
        pin(app); // opaque call
    }

    return res;
}

// ==== join transformer ====

bool Divergence::is_homogeneous(const Def* p) const {
    auto it = node2graph_.find(p);
    if (it == node2graph_.end()) return true;
    if (auto i = it->second->sync_deps.find(p); i != it->second->sync_deps.end())
        return std::ranges::none_of(i->second, [&](const Def* b) { return is_varying(b); });
    return true;
}

bool Divergence::receives_varying(const Def* p, size_t i, size_t n) const {
    auto& g = *node2graph_.at(p);
    if (g.opaque_conts.contains(p)) return true;
    if (auto args = g.incoming.find(p); args != g.incoming.end())
        for (auto arg : args->second)
            if (is_varying(arg->proj(n, i))) return true;
    return false;
}

void Divergence::join(Lam* p) {
    auto it = node2graph_.find(p);
    if (it == node2graph_.end() || it->second->cyclic) return; // skip cyclic

    auto& g  = *it->second;
    bool hom = is_homogeneous(p);
    auto n   = p->num_tvars();

    for (size_t i = 0; i != n; ++i) {
        if (is_varying(p->tvar(i))) continue;
        if (receives_varying(p, i, n)) { // edge effect: a varying value flows in
            pin_tvar(p, i);
            continue;
        }
        if (hom) continue; // all relevant splits are uniform

        // Definate Reaching: if every edge passes the very same Def and p does not nest it, that Def definitely
        // reaches p. Then local inhomogeneity at p does not matter: all threads agree on it anyway.
        const Def* def = nullptr;
        bool same      = true;
        if (auto args = g.incoming.find(p); args != g.incoming.end())
            for (auto arg : args->second) {
                auto a = arg->proj(n, i);
                if (!def) def = a;
                same &= a == def;
            }

        if (!same || !def || p->nests(def)) pin_tvar(p, i);
    }
}
// dumps
//

void Divergence::dump(std::ostream& os) {
    auto fact = [&](const Def* def) { return is_varying(def) ? "varying" : "uniform"; };

    for (const auto& g : graphs_) {
        std::println(os, "{}:", g.root->unique_name());
        for (auto n : g.topo) {
            auto lam = isa_block(n);
            if (!lam || lam == g.root) continue;

            std::print(os, "    {}(", lam->unique_name());
            for (size_t i = 0, e = lam->num_tvars(); i != e; ++i)
                std::print(os, "{}{}: {}", i ? ", " : "", lam->tvar(i)->unique_name(), fact(lam->tvar(i)));
            std::print(os, ")");
            if (auto i = g.splits.find(lam); i != g.splits.end())
                std::print(os, "   split on {}: {}", i->second->unique_name(), fact(i->second));
            if (auto i = g.sync_deps.find(lam); i != g.sync_deps.end()) {
                std::print(os, "   splits:");
                for (auto b : i->second)
                    std::print(os, " {}", b->unique_name());
            }
            std::println(os);
        }
    }
}

void Divergence::dump_control(std::ostream& os) {
    std::println(os, "ControlGraph: ");
    for (const auto& g : graphs_) {
        std::println(os, "{}:", g.root->unique_name());
        for (auto n : g.topo) {
            std::print(os, "    {} ->", n->unique_name());
            if (auto i = g.succs.find(n); i != g.succs.end())
                for (auto s : i->second)
                    std::print(os, " {}", s->unique_name());
            if (auto lam = n->isa_mut<Lam>(); lam && g.splits.contains(lam))
                std::print(os, "   [split on {}]", g.splits.at(lam)->unique_name());
            std::println(os);
        }
    }
}
} // namespace mim
