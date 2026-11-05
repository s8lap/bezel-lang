//! case: s26_5_2307
//! doc: spec
//! section: 26.5
//! title: a user extension Count(int) on SortedSet is never consulted
//! rule: extension blocks are **never consulted** for that name on that
//!   receiver
//! cites: 26.5, 26.1
//! expect: reject
//! diagnostic: instance member
//! issue: 7812
//! notes: Red on base by diagnostic only: the pre-fix compiler also failed
//!   this program, but in codegen with an IR verifier message (Load operand
//!   must be a pointer.) rather than the 26.5 diagnostic citing the
//!   instance member, the staging tip reports the instance member.
module Conf;

extension SortedSet<T>
  public method Count(n: int): int
    return 99;
  end Count;
end;

public class Program
  public static method Main()
    var s: SortedSet<int> := new SortedSet<int>();
    s.Add(1);
    s.Add(2);
    s.Add(3);
    Console.WriteLine(s.Count(2));
  end Main;
end Program;
