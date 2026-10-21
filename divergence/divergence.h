#pragma once

#include <cstdint>

#include <mim/phase.h>

namespace mim {

/// Divergence (uniformity) analysis after Rosemann, Moll, and Hack:
/// [An Abstract Interpretation for SPMD Divergence on Reducible Control Flow Graphs]
///
/// Analysis::lattice per Def:
/// ```
///  ⊤  varying
///  |
///  ⊥  uniform (optimistic)
/// ```
/// Sources of divergence: the results of opaque calls
///
class Divergence : public mim::Analysis {
public:
    Divergence(World& world)
        : mim::Analysis(world, "divergence") {}

    void run_to_fp();

    bool is_varying(const Def* def) const { return is_top(def); }
    bool is_uniform(const Def* def) const { return !is_varying(def); }

    void dump(std::ostream&);
    void dump_control(std::ostream&);

private:
    /// The control-flow graph of the continuations of one external function, derived from the IR:
    /// `q → p` iff `q`'s body jumps to, dispatches to, or passes as continuation the node `p`.
    /// A node is either a basic-block Lam or an exit
    struct ControlGraph {
        Lam* root   = nullptr;
        bool cyclic = false;
        DefVec topo;               // All nodes in topological order.
        DefMap<DefVec> succs;      // Successor nodes.
        DefMap<DefVec> preds;      // Predecessor nodes.
        DefMap<DefVec> incoming;   // All arguments a node receives
        DefMap<DefVec> sync_deps;  // Sync dependences: `splits(p)`
        LamMap<const Def*> splits; // Split node → its branch variable.
        DefSet opaque_conts;
    };

    enum nodeStates : uint8_t { Unvisited, Active, Done };

    void prepare() final;
    const Def* rewrite_imm(const Def*) final;

    void build(ControlGraph&);

    /// Uniformity of the i-th incoming argument of node @p p.
    bool is_varying_targ(const Def* p, size_t i, size_t n) const;
    void pin_tvar(Lam* lam, size_t i);

    std::deque<ControlGraph> graphs_;
    DefMap<ControlGraph*> node2graph_;
};

} // namespace mim
