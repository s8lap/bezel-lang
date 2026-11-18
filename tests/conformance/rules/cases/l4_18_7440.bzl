//! case: l4_18_7440
//! doc: stdlib
//! section: 4.18
//! title: First and Last of a SortedSet<string> read through a nullable carrier give the least and
//!   greatest
//! rule: A comparison-ordered set: membership by `CompareTo`-equality, iteration in ascending order.
//! cites: 4.18, 9.6
//! expect: run
//! output: a
//!   b
module Conf;
public class Program
  public static method Main()
    using var s := new SortedSet<string>();
    s.Add("b"); s.Add("a");
    var p: SortedSet<string>? := s;
    Console.WriteLine(p?.First ?? "none");
    Console.WriteLine(p?.Last ?? "none");
  end Main;
end Program;
