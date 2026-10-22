//! case: s28_7_0010
//! doc: spec
//! section: 28.7
//! title: reassigning a Set<int> local leaves the new set holding only the new elements
//! rule: "Assignment to an owned field or owned local disposes the displaced value."
//! cites: 4.10
//! expect: run
//! output: 1
//!   True
//!   False
module Conf;

public class Program
  public static method Main()
    var s := new Set<int>();
    s.Add(1);
    s.Add(2);
    s := new Set<int>();
    s.Add(3);
    Console.WriteLine(s.Count);
    Console.WriteLine(s.Contains(3));
    Console.WriteLine(s.Contains(1));
  end Main;
end Program;
