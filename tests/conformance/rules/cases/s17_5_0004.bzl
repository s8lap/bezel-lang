//! case: s17_5_0004
//! doc: spec
//! section: 17.5
//! title: a nullable left operand is admitted where the selected Contains parameter is itself nullable
//! rule: "Where the selected `Contains` parameter is itself nullable —
//!   `Array<int?>`'s `Contains(item: int?)` ... — a nullable left operand is
//!   admitted: the test performed is the null-safe equality of §8.4."
//! cites: 17.5, 8.4
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var xs: Array<int?> := [1, null, 3];
    var n: int? := null;
    var found := n in xs;
    Console.WriteLine(found);
  end Main;
end Program;
