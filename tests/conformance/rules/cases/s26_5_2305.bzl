//! case: s26_5_2305
//! doc: spec
//! section: 26.5
//! title: a user extension on SortedSet whose name the type does not
//!   declare is consulted
//! rule: If the receiver's type declares no instance member of that name,
//!   the compiler searches the current module and all imported modules
//! cites: 26.5, 26.1
//! expect: run
//! output: 99
//! notes: Accepting twin of s26_5_2304 (green on base and tip).
module Conf;

extension SortedSet<T>
  public method CountWhere(p: (T) -> bool): int
    return 99;
  end CountWhere;
end;

public class Program
  public static method Main()
    var s: SortedSet<int> := new SortedSet<int>();
    s.Add(1);
    s.Add(2);
    s.Add(3);
    Console.WriteLine(s.CountWhere(lambda(x: int) => x > 1));
  end Main;
end Program;
