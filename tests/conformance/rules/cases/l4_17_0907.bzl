//! case: l4_17_0907
//! doc: stdlib
//! section: 4.17
//! title: SortedDictionary.GetOrNull returns the value when the key is present and null when it is absent
//! rule: (§1 *OrNull tier): absent -> null; present -> the
//! cites: 4.17
//! expect: run
//! output: 7
//!   True
module Conf;

public class Program
  public static method Main()
    using var d := new SortedDictionary<string, int>();
    d.Add("a", 7);
    Console.WriteLine(d.GetOrNull("a")!);
    Console.WriteLine(d.GetOrNull("b") = null);
  end Main;
end Program;
