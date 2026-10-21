//! case: s17_5_0016
//! doc: spec
//! section: 17.5
//! title: a nullable int left operand over an Array<int> is rejected
//! rule: "Where the selected `Contains` declares a **non-nullable** parameter,
//!   a nullable left operand is a **compile error** (`T?` does not implicitly
//!   convert to `T`, [§5.1])" ([§17.5]).
//! cites: 17.5, 5.1
//! expect: reject
//! diagnostic: nullable
//! issue: 6535
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2];
    var n: int? := 1;
    var found := n in a;
    Console.WriteLine(found);
  end Main;
end Program;
