//! case: s8_6_4709
//! doc: spec
//! section: 8.6
//! title: a narrowed int? boxed to IComparable<int> carries the present int
//! rule: If the value is non-null, the result is a boxed `T` (the non-nullable form). The nullable
//!   wrapper is stripped during boxing.
//! cites: 5.1, 7.4, 2.19
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    var n: int? := 7;
    var g: IComparable<int> := n;
    Console.WriteLine(g.CompareTo(7));
  end Main;
end Program;
