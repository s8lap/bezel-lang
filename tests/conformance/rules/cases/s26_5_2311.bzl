//! case: s26_5_2311
//! doc: spec
//! section: 26.5
//! title: a predicate Count after Where reaches the extension
//! rule: If the receiver's type declares no instance member of that name,
//!   the compiler searches the current module and all imported modules
//! cites: 5, 26.5
//! expect: run
//! output: 1
//! notes: Accepting control (green on base and tip): Where yields an
//!   IEnumerable<int>, which declares no Count.
module Conf;

public class Program
  public static method Main()
    var s: SortedSet<int> := new SortedSet<int>();
    s.Add(1);
    s.Add(2);
    s.Add(3);
    Console.WriteLine(s.Where(lambda(x: int) => x > 1).Count(lambda(x: int) => x > 2));
  end Main;
end Program;
