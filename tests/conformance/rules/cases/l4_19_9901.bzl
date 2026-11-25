//! case: l4_19_9901
//! doc: stdlib
//! section: 4.19
//! title: assigning an element of a stdlib FrozenArray reached through an alias is rejected
//! rule: deeply immutable after construction
//! cites: 4.19, 23.4
//! expect: reject
//! diagnostic: FrozenArray
//! issue: 9251
//! notes: Harvested from the #9251 review (b_alias), the rejecting twin of s2_3_9904. No user type named
//!   FrozenArray exists here, so the alias names the stdlib type, whose indexer is get-only (stdlib
//!   4.19).
module Conf;

type FA := FrozenArray<int>;

public class Program
  public static method Main()
    var s := new Array<int>();
    s.Add(7);
    using var f: FA := FrozenArray<int>.From(s);
    f[0] := 5;
  end Main;
end Program;
