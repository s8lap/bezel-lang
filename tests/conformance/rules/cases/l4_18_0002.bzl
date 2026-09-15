//! case: l4_18_0002
//! doc: stdlib
//! section: 4.18
//! title: Add on an already-present element is a silent no-op
//! rule: As with Set<T>, adding an equal element leaves the set unmodified
//!   -- Add returns () and discards silently.
//! cites: 4.18
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    using var ss := new SortedSet<int>();
    ss.Add(1);
    ss.Add(1);
    Console.WriteLine(ss.Count);
  end Main;
end Program;
