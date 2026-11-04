//! case: s22_15_0041
//! doc: spec
//! section: 22.15
//! title: a pure body may not call a LINQ-style extension such as Any on an Array
//! rule: A call — method, operator, accessor read, or constructor in the
//!   admitted positions — is permitted iff the member declared on the
//!   receiver's static type (or the statically resolved member, for statics
//!   and extensions) is `pure`-declared or pure-classified (below).
//! cites: 22.15
//! expect: reject
//! diagnostic: pure
//! issue: 7453
module Conf;

public class Ops
  public static pure method AnyPositive(a: Array<int>): bool => a.Any(lambda(x: int) => x > 0);
end Ops;
