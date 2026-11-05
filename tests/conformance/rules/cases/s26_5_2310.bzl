//! case: s26_5_2310
//! doc: spec
//! section: 26.5
//! title: a predicate Count on a plain IEnumerable receiver reaches the
//!   extension
//! rule: If the receiver's type declares no instance member of that name,
//!   the compiler searches the current module and all imported modules
//! cites: 5, 26.5
//! expect: run
//! output: 2
//! notes: Accepting control (green on base and tip): the receiver's
//!   DECLARED type IEnumerable<int> declares no Count (stdlib 4.12), so the
//!   LINQ predicate Count is reachable even though the underlying object is
//!   a SortedSet.
module Conf;

public class Program
  public static method Main()
    var s: SortedSet<int> := new SortedSet<int>();
    s.Add(1);
    s.Add(2);
    s.Add(3);
    var e: IEnumerable<int> := s;
    Console.WriteLine(e.Count(lambda(x: int) => x > 1));
  end Main;
end Program;
