//! case: s26_5_9005
//! doc: spec
//! section: 26.5
//! title: a `?.` read of a name nothing supplies on FrozenArray<int> is a compile error at the name
//! rule: nothing supplies it: the access names no member and is a compile error at the name, the diagnostic
//!   naming the receiver's static type
//! cites: 7.5, 7.8
//! expect: reject
//! diagnostic: no member
//! notes: Harvested from the #8783 review sweep (s2_tip.txt, row `FrozenArray<int> Bogus nc`). Spec 26.5 rule
//!   2 applies to every member access with a `?.` receiver read at its underlying non-nullable type
//!   (7.5). `Bogus` is declared by no instance member of FrozenArray<int> and by no extension in scope,
//!   so nothing supplies it. The accepting twin is s7_5_9005 (documented members via `?.`).
module Conf;
public class Program
  public static method Main()
    var q: FrozenArray<int>? := null;
    var x := q?.Bogus;
  end Main;
end Program;
