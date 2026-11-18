//! case: l4_18_7438
//! doc: stdlib
//! section: 4.18
//! title: SortedSet<string> First and Last follow removal and re-insertion down to empty
//! rule: A comparison-ordered set: membership by `CompareTo`-equality, iteration in ascending order.
//! cites: 4.18, 9.6
//! expect: run
//! output: b
//!   b
//!   z
//!   threw
module Conf;
public class Program
  public static method Main()
    using var s := new SortedSet<string>();
    s.Add("b"); s.Add("a"); s.Add("c");
    s.Remove("a"); s.Remove("c");
    Console.WriteLine(s.First); Console.WriteLine(s.Last);
    s.Add("z");
    Console.WriteLine(s.Last);
    s.Remove("b"); s.Remove("z");
    try
      Console.WriteLine(s.First);
    catch InvalidOperationException => Console.WriteLine("threw");
    end;
  end Main;
end Program;
