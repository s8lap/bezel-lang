//! case: s26_5_2306
//! doc: spec
//! section: 26.5
//! title: Count with a non-predicate argument on a SortedSet is refused, no
//!   extension fallback
//! rule: the call is a compile error citing the instance member(s); it does
//!   not fall back to an extension
//! cites: 5, 26.5
//! expect: reject
//! diagnostic: instance member
//! issue: 7812
//! notes: Red on base by diagnostic only: the pre-fix compiler also failed
//!   this program, but in codegen with an IR verifier message (Load operand
//!   must be a pointer.) rather than the 26.5 diagnostic citing the
//!   instance member, the staging tip reports the instance member.
module Conf;

public class Program
  public static method Main()
    var s: SortedSet<int> := new SortedSet<int>();
    s.Add(1);
    s.Add(2);
    s.Add(3);
    Console.WriteLine(s.Count(2));
  end Main;
end Program;
