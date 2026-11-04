//! case: s30_12_0013
//! doc: spec
//! section: 30.12
//! title: a mutable static of a data-owning lock type is rejected
//! rule: They are deliberately not in rule 2's form-(i) set: a *mutable* static
//!   of a lock type remains rejected
//! cites: 30.12, 22.12
//! expect: reject
//! diagnostic: mutable static
//! issue: 1721
//! notes: the `static owned readonly` spelling of the same field is the
//!   warning-free registry pinned by l6_11_0007 and s22_12_0047.
module Conf;

import System.Threading;

public sealed class Counter
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Counter;

public static class Registry
  private static owned _reg: Locked<Counter> := new Locked<Counter>(new Counter(0));
end Registry;
