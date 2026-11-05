//! case: s26_5_2301
//! doc: spec
//! section: 26.5
//! title: a predicate Count on a SortedSet is refused, the instance
//!   property blocks the extension
//! rule: the call is a compile error citing the instance member(s); it does
//!   not fall back to an extension
//! cites: 5, 26.5
//! expect: reject
//! diagnostic: instance member
//! issue: 7812
//! notes: Red on base: the pre-fix compiler accepted the plain `.` spelling
//!   of the predicate Count on this receiver (it fell through to the LINQ
//!   extension), the staging tip refuses it.
module Conf;

public class Program
  public static method Main()
    var s: SortedSet<int> := new SortedSet<int>();
    s.Add(1);
    s.Add(2);
    s.Add(3);
    Console.WriteLine(s.Count(lambda(x: int) => x > 1));
  end Main;
end Program;
