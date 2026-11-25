//! case: l4_10_9971
//! doc: stdlib
//! section: 4.10
//! title: a Set<string?> collapses an equal string built at run time and an equal literal
//! rule: public method TryAdd(item: T): bool; // returns whether added
//! cites: 4.10, 3.6, 8.4
//! expect: run
//! issue: 9277
//! output: True
//!   False
//!   True
//!   False
//!   2
module Conf;

public class Program
  public static method Main()
    var s := new Set<string?>();
    var head := "a";
    Console.WriteLine(s.TryAdd("ab"));
    Console.WriteLine(s.TryAdd(head + "b"));
    Console.WriteLine(s.TryAdd("cd"));
    Console.WriteLine(s.TryAdd("cd"));
    Console.WriteLine(s.Count);
  end Main;
end Program;
