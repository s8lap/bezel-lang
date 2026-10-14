//! case: s7_4_0047
//! doc: spec
//! section: 7.4
//! title: a narrowing is invalidated by a reassignment that reaches the loop head on the back-edge
//! rule: Narrowing is flow-sensitive: at each program point the compiler tracks, per local variable ... whether its value is known non-null. (v0.338 §7.4 line 898)
//!   The narrowing of a variable is invalidated when, within the controlled block: ... the variable is reassigned from a nullable-typed expression (v0.338 §7.4 lines 906, 908). The reassignment is below the read in the loop body and reaches the read again on the back-edge (derived, see README).
//! cites: 12.7
//! expect: reject
//! issue: 6114
//! diagnostic: nullable
module Conf;

// nullable form of #6114 (same loop-head mechanism, HYPOTHESIS)
public class Program
  public static method Probe(s: string?, n: string?)
    if s <> null then
      var i := 0;
      while i < 2 do
        Console.WriteLine(s.Length);
        s := n;
        i := i + 1;
      end while;
    end if;
  end Probe;

  public static method Main()
    Probe("ab", null);
  end Main;
end Program;
