//! case: s26_5_2304
//! doc: spec
//! section: 26.5
//! title: a user extension Count(predicate) on SortedSet is never consulted
//! rule: extension blocks are **never consulted** for that name on that
//!   receiver
//! cites: 26.5, 26.1
//! expect: reject
//! diagnostic: instance member
//! issue: 7812
//! notes: Red on base: the pre-fix compiler accepted the plain `.` spelling
//!   of the predicate Count on this receiver (it fell through to the LINQ
//!   extension), the staging tip refuses it. (On base it printed 99, the
//!   user extension.)
module Conf;

extension SortedSet<T>
  public method Count(p: (T) -> bool): int
    return 99;
  end Count;
end;

public class Program
  public static method Main()
    var s: SortedSet<int> := new SortedSet<int>();
    s.Add(1);
    s.Add(2);
    s.Add(3);
    Console.WriteLine(s.Count(lambda(x: int) => x > 1));
  end Main;
end Program;
