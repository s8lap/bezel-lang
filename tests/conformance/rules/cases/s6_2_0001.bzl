//! case: s6_2_0001
//! doc: spec
//! section: 6.2
//! title: mixed int/long arithmetic promotes the int operand and the result has the wider type
//! rule: When operands have different types, the smaller-range operand is
//!   implicitly promoted to the larger type. The result has the larger type.
//! cites: 6.2
//! expect: run
//! stdout: 4000000000
module Conf;

public class Program
  public static method Main()
    var n: int := 2_000_000_000;
    var w: long := 2_000_000_000;
    var r := n + w;
    Console.WriteLine(r);
  end Main;
end Program;
