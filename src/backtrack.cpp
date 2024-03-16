#include "internal.hpp"
#include <vector>

namespace CaDiCaL {

// The global assignment stack can only be (partially) reset through
// 'backtrack' which is the only function using 'unassign' (inlined and thus
// local to this file).  It turns out that 'unassign' does not need a
// specialization for 'probe' nor 'vivify' and thus it is shared.

inline void Internal::unassign (int lit) {
  assert (val (lit) > 0);
  set_val (lit, 0);
  if (var (lit).missed_implication)
    LOG (var (lit).missed_implication, "deleting missed %d on level %d (original: %d)", lit, var (lit).missed_level, var (lit).level);
  var (lit).missed_implication = nullptr;
  var (lit).missed_level = -1;
  var (lit).dirty = false;

  int idx = vidx (lit);
  LOG ("unassign %d @ %d", lit, var (lit).level);
  num_assigned--;

  // In the standard EVSIDS variable decision heuristic of MiniSAT, we need
  // to push variables which become unassigned back to the heap.
  //
  if (!scores.contains (idx))
    scores.push_back (idx);

  // For VMTF we need to update the 'queue.unassigned' pointer in case this
  // variable sits after the variable to which 'queue.unassigned' currently
  // points.  See our SAT'15 paper for more details on this aspect.
  //
  if (queue.bumped < btab[idx])
    update_queue_unassigned (idx);
}

struct missed_level_rank {
  Internal *internal;
  missed_level_rank (Internal *i) : internal (i) {}
  typedef uint64_t Type;
  Type operator() (const int &a) const { return internal->var (a).level; }
};

struct missed_level_smaller {
  Internal *internal;
  missed_level_smaller (Internal *i) : internal (i) {}
  bool operator() (const int &a, const int &b) const {
    const auto s = missed_level_rank (internal) (a);
    const auto t = missed_level_rank (internal) (b);
    return s < t;
  }
};
/*------------------------------------------------------------------------*/

// Update the current target maximum assignment and also the very best
// assignment.  Whether a trail produces a conflict is determined during
// propagation.  Thus that all functions in the 'search' loop after
// propagation can assume that 'no_conflict_until' is valid.  If a conflict
// is found then the trail before the last decision is used (see the end of
// 'propagate').  During backtracking we can then save this largest
// propagation conflict free assignment.  It is saved as both 'target'
// assignment for picking decisions in 'stable' mode and if it is the
// largest ever such assignment also as 'best' assignment. This 'best'
// assignment can then be used in future stable decisions after the next
// 'rephase_best' overwrites saved phases with it.

void Internal::update_target_and_best () {

  bool reset = (rephased && stats.conflicts > last.rephase.conflicts);

  if (reset) {
    target_assigned = 0;
    if (rephased == 'B')
      best_assigned = 0; // update it again
  }

  if (no_conflict_until > target_assigned) {
    copy_phases (phases.target);
    target_assigned = no_conflict_until;
    LOG ("new target trail level %zu", target_assigned);
  }

  if (no_conflict_until > best_assigned) {
    copy_phases (phases.best);
    best_assigned = no_conflict_until;
    LOG ("new best trail level %zu", best_assigned);
  }

  if (reset) {
    report (rephased);
    rephased = 0;
  }
}

/*------------------------------------------------------------------------*/

void Internal::backtrack (int new_level) {
  assert (missed_props.empty());
  assert (new_level >= 0);
  assert (new_level <= level);
  if (new_level == level)
    return;

  stats.backtracks++;
  update_target_and_best ();

  assert (num_assigned == trail.size ());

  const size_t assigned = control[new_level + 1].trail;

  LOG ("backtracking to decision level %d with decision %d and trail %zd",
       new_level, control[new_level].decision, assigned);

  const size_t end_of_trail = trail.size ();
  size_t i = assigned, j = i;

#ifdef LOGGING
  int unassigned = 0;
#endif
  int reassigned = 0;

  notify_backtrack (new_level);
  if (external_prop && !external_prop_is_lazy && notified > assigned) {
    LOG ("external propagator is notified about some unassignments (trail: "
         "%zd, notified: %zd).",
         trail.size (), notified);
    notified = assigned;
  }

  const size_t default_dirty = (size_t) -1;
  size_t earliest_dirty = default_dirty;
  const bool strongchrono = (opts.chrono >= 3);

  while (i < end_of_trail) {
    int lit = trail[i++];
    Var &v = var (lit);
    if (strongchrono && v.missed_implication && v.level > new_level && v.missed_level <= new_level) {
      LOG (v.missed_implication,
           "BT missed lower-level implication of %d at level %d (was %d)",
           lit, var (lit).missed_level, var (lit).level);
      assert (!v.missed_implication->moved);
      LOG (v.reason, "other reason");
#ifdef DEBUG
      for (auto other : *v.missed_implication) {
        LOG ("lit %d at level %d", other, var (other).level);
        if (other != lit)
          assert (val (other) < 0);
      }
#endif
      missed_props.push_back (lit);
    }
    else if (v.level > new_level) {
      unassign (lit);
#ifdef LOGGING
      unassigned++;
#endif
    } else {
      // This is the essence of the SAT'18 paper on chronological
      // backtracking.  It is possible to just keep out-of-order assigned
      // literals on the trail without breaking the solver (after some
      // modifications to 'analyze' - see 'opts.chrono' guarded code there).
      // 
      // With strong chrono, this still happens for unit clauses, where we do not have a reason, so
      // we cannot mark it as missed.
      assert (opts.chrono || external_prop || did_external_prop);
#ifdef LOGGING
      if (!v.level)
        LOG ("reassign %d @ 0 unit clause %d (dirty: %d)", lit, lit, var (lit).dirty);
      else
        LOG (v.reason, "reassign %d @ %d", lit, v.level);
#endif
      trail[j] = lit;
      v.trail = j++;
      reassigned++;
      if (strongchrono && v.dirty && earliest_dirty == default_dirty) {
        LOG ("found dirty literal %d at %" PRId64, lit, j - 1);
        assert (j >= 1);
        earliest_dirty = j - 1;
      }
    }
  }
  trail.resize (j);
  LOG ("unassigned %d literals %.0f%%", unassigned,
       percent (unassigned, unassigned + reassigned));
  LOG ("reassigned %d literals %.0f%%", reassigned,
       percent (reassigned, unassigned + reassigned));

  if (propagated > assigned)
    propagated = assigned;
  if (propagated2 > assigned)
    propagated2 = assigned;
  if (no_conflict_until > assigned)
    no_conflict_until = assigned;

  propergated = 0; // Always go back to root-level.

  assert (notified <= assigned + reassigned);
  if (reassigned) {
    notify_assignments ();
  }

  control.resize (new_level + 1);
  level = new_level;
  if (tainted_literal) {
    assert (opts.ilb);
    if (!val (tainted_literal)) {
      tainted_literal = 0;
    }
  }

  if (strongchrono) {
    if (earliest_dirty == default_dirty)
      earliest_dirty = j;
    propagated = earliest_dirty;
    propagated2 = earliest_dirty;
    no_conflict_until = earliest_dirty;
    stats.avoidedprops += reassigned;
  }
  
  if (strongchrono) {
    // Here we slowly bubble down the literals: they remain on current level
    // with a missed propagation until reaching their final position.
    // It is only once reached the final position that they do get real
    // units
#if 0
    MSORT (opts.radixsortlim, begin (missed_props), end (missed_props), missed_level_rank(this), missed_level_smaller(this));
#endif
    for (int i = missed_props.size() - 1; i >= 0; --i) {
      const int lit = missed_props[i];
      Var &v = var (lit);
      assert (v.missed_implication);
      ++stats.missedprops;
      assert (val (lit) > 0);
      assert (val (-lit) < 0);
      v.reason = v.missed_implication;
      v.dirty = true; // necessary if a conflict is immediately found before repropagating that literal
      const bool new_unit = (!v.missed_level);
      std::vector<uint64_t> lrat_chain_tmp;
      if (new_unit && !unsat) {
	if (lrat) { // build units early, but still keep them as missed
	  // can be called during conflict analysis, so we need to copy the lrat_chain
	  lrat_chain_tmp = (std::move (lrat_chain));
	  lrat_chain.clear();
	}
	build_chain_for_units (lit, v.missed_implication, true);
	learn_unit_clause (lit);
	if (lrat) {
	  lrat_chain = std::move (lrat_chain_tmp);
	  // not marking the clause garbage, because it can be involved in the conflict analysis
	  LOG (lrat_chain, "chain set back to: ");
	}
	v.reason = 0;
      }
      assert (level >= v.missed_level);
      v.level = v.missed_level;
      assert (new_level >= v.missed_level);
      v.trail = trail.size();
      if (v.missed_level)
	LOG (v.reason,
             "BT setting missed propagation lit %d at level %d with reason", lit, v.level);
      else {
	LOG ("BT setting missed propagation lit %d to root level", lit);
      }
      trail.push_back (lit);
      var (lit).missed_implication = nullptr;
    }
    missed_props.clear();
    if (!missed_props.empty ())
      notify_assignments ();
  }

  assert (num_assigned == trail.size ());
  assert (propagated <= trail.size());
}

} // namespace CaDiCaL
