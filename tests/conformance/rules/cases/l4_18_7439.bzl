//! case: l4_18_7439
//! doc: stdlib
//! section: 4.18
//! title: adding a duplicate to a SortedSet<string> leaves it unmodified and the empty string is least
//! rule: adding an equal element leaves the set unmodified
//! cites: 4.18, 9.6
//! expect: run
//! output: 3
//!   0
//!   b
module Conf;
public class Program
  public static method Main()
    using var s := new SortedSet<string>();
    s.Add("b"); s.Add("b"); s.Add("a"); s.Add("");
    Console.WriteLine(s.Count);
    var e := s.First;
    Console.WriteLine(e.Length);
    Console.WriteLine(s.Last);
  end Main;
end Program;
