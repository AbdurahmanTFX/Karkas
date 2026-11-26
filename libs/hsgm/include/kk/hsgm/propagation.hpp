#pragma once
// propagation (ADR 0049): how far a change can spread, by the visibility matrix of the dependency
// structure — MacCormack, Baldwin, Rusnak 2012, §3.1: V = Σ_{n=0..N} M^n, propagation cost = density of V.
#include <string>
#include <utility>
#include <vector>

#include "kk/hsgm/input.hpp"

namespace kk::hsgm {

struct Propagation {
  int n = 0;                 // code blocks
  long long transitive = 0;  // entries of V: no contracts, a change reaches everything downstream
  long long direct = 0;      // entries of I ∨ M: complete contracts, the changed contract reaches its direct dependents
                             // self = n: complete contracts, the contract kept, only the block itself
  std::vector<std::pair<std::string, int>> reach;  // block → blocks a change to it can affect (itself included),
                                                   // largest first, then by id
};

Propagation propagation(const Input& in);
std::string format(const Propagation& p);

}  // namespace kk::hsgm
