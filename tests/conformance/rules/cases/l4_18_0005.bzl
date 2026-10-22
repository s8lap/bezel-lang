//! case: l4_18_0005
//! doc: stdlib
//! section: 4.18
//! title: reassigning a SortedSet<int> local leaves the new set in ascending order
//! rule: "A comparison-ordered set: membership by `CompareTo`-equality, iteration in ascending order."
//! cites: 28.7
//! expect: run
//! output: 1
//!   2
//!   3
module Conf;

public class Program
  public static method Main()
    var ss := new SortedSet<int>();
    ss.Add(5);
    ss.Add(4);
    ss := new SortedSet<int>();
    ss.Add(3);
    ss.Add(1);
    ss.Add(2);
    for var x in ss do
      Console.WriteLine(x);
    end;
  end Main;
end Program;
