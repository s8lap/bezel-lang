//! case: s17_8_0001
//! doc: spec
//! section: 17.8
//! title: x! asserts non-null and yields the non-nullable form of the type
//! rule: "`x!` asserts that `x` is non-null. The result type is the
//!   non-nullable form of the expression's type."
//! cites: 17.8
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var n: int? := 5;
    var m: int := n!;
    Console.WriteLine(m);
  end Main;
end Program;
