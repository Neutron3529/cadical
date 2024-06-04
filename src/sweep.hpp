#ifndef _sweep_hpp_INCLUDED
#define _sweep_hpp_INCLUDED

namespace CaDiCaL {

struct Internal;

struct sweep_proof_clause {
  uint64_t id;
  vector<int> literals;
  // for lrat
  unsigned cid;  // kitten id
  bool learned;
  vector<unsigned> chain;
};

struct Sweeper {
  Sweeper (Internal *internal);
  ~Sweeper ();
  Internal *internal;
  Random random;
  vector<unsigned> depths;
  int* reprs;
  vector<int> next, prev;
  int first, last;
  unsigned encoded;
  unsigned save;
  vector<int> vars;
  vector<Clause *> clauses;
  vector<int> clause;
  vector<int> propagate;
  vector<int> backbone;
  vector<int> partition;
  vector<sweep_proof_clause> core[2];
  struct {
    uint64_t ticks;
    unsigned clauses, depth, vars;
  } limit;
};

} // namespace CaDiCaL

#endif
