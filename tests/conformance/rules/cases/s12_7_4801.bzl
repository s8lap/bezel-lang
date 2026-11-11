//! case: s12_7_4801
//! doc: spec
//! section: 12.7
//! title: is string is false for a stdlib collection held in an Object slot and true for a string
//! rule: `is T` tests whether a value is an instance of `T` and produces `bool`.
//! cites: 12.7, 8.1
//! expect: run
//! output: other
//!   other
//!   string
module Conf;

public class Program
  public static method Kind(o: Object): string
    return (if o is string then "string" else "other");
  end Kind;

  public static method Main()
    using var d := new Dictionary<int, int>();
    using var l := new LinkedList<int>();
    var t := "str";
    Console.WriteLine(Kind(d));
    Console.WriteLine(Kind(l));
    Console.WriteLine(Kind(t));
  end Main;
end Program;
