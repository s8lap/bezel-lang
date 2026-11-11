//! case: s8_6_4710
//! doc: spec
//! section: 8.6
//! title: a non-nullable Guid boxed to IComparable<Guid> keeps its value
//! rule: If the value is non-null, the result is a boxed `T` (the non-nullable form). The nullable
//!   wrapper is stripped during boxing.
//! cites: 5.1, 2.6
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    var n: Guid := Guid.Empty;
    var g: IComparable<Guid> := n;
    Console.WriteLine(g.CompareTo(Guid.Empty));
  end Main;
end Program;
