//! case: l4_18_0001
//! doc: stdlib
//! section: 4.18
//! title: SortedSet<T> iterates in ascending order regardless of insertion order
//! rule: A comparison-ordered set: membership by CompareTo-equality,
//!   iteration in ascending order.
//! cites: 4.18
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    using var ss := new SortedSet<int>();
    ss.Add(3);
    ss.Add(1);
    ss.Add(2);
    for var x in ss do
      Console.WriteLine(x);
    end;
  end Main;
end Program;
