//! case: l4_19_9902
//! doc: stdlib
//! section: 4.19
//! title: a compound assignment through a stdlib FrozenArray indexer is rejected
//! rule: deeply immutable after construction
//! cites: 4.19, 6.6, 23.4
//! expect: reject
//! diagnostic: FrozenArray
//! issue: 9251
//! notes: Harvested from the #9251 review (b_pluseq), the rejecting twin of s2_3_9903. `f[0] += 5` expands
//!   to a store through the get-only indexer.
module Conf;

public class Program
  public static method Main()
    var s := new Array<int>();
    s.Add(7);
    using var f := FrozenArray<int>.From(s);
    f[0] += 5;
  end Main;
end Program;
